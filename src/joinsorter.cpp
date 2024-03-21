//
// Copyright (c) 2024, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "joinsorter.h"

#include "std/hash.h"
#include "std/openhash.h"
#include "sphinxquery.h"
#include "sphinxsort.h"
#include "sphinxjson.h"
#include "querycontext.h"
#include "docstore.h"

static int64_t g_iJoinCacheSize = 20971520;


void SetJoinCacheSize ( int64_t iSize )
{
	g_iJoinCacheSize = iSize;
}


int64_t GetJoinCacheSize()
{
	return g_iJoinCacheSize;
}


static bool GetJoinAttrName ( const CSphString & sAttr, const CSphString & sJoinedIndex, CSphString * pModified = nullptr )
{
	CSphString sPrefix;
	sPrefix.SetSprintf ( "%s.", sJoinedIndex.cstr() );
	int iPrefixLen = sPrefix.Length();

	bool bRightTable = false;
	CSphString sMod = sAttr;
	const char * szStart = sMod.cstr();
	while ( true )
	{
		const char * szFound = strstr ( sMod.cstr(), sPrefix.cstr() );
		if ( !szFound )
			break;

		if ( szFound > szStart )
		{
			char c = *(szFound-1);
			if ( ( c>='0' && c<='9' ) || ( c>='a' && c<='z' ) || ( c>='A' && c<='Z' ) || c=='_' )
				continue;		
		}

		bRightTable = true;
		int iStart = szFound-sMod.cstr();
		CSphString sNewExprPre = iStart > 0 ? sMod.SubString ( 0, iStart ) : "";

		int iPostLen = sMod.Length()-iStart-iPrefixLen;
		CSphString sNewExprPost = iPostLen > 0 ? sMod.SubString ( iStart + iPrefixLen, sMod.Length()-iStart-iPrefixLen ) : "";
		sMod.SetSprintf ( "%s%s", sNewExprPre.cstr(), sNewExprPost.cstr() );
	}

	if ( bRightTable )
	{
		if ( pModified )
			*pModified = sMod;

		return true;
	}

	return false;
}


static StrVec_t ParseGroupBy ( const CSphString & sGroupBy )
{
	StrVec_t dRes;
	sphSplit ( dRes, sGroupBy.cstr(), ", \t\n" );
	return dRes;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// FIXME! maybe replace it with a LRU cache
class MatchCache_c
{
public:
						MatchCache_c ( uint64_t uCacheSize );
						~MatchCache_c();

	void				SetSchema ( const ISphSchema * pSchema );
	bool				Add ( uint64_t uHash, const CSphSwapVector<CSphMatch> & dMatches );
	FORCE_INLINE bool	Fetch ( uint64_t uHash, CSphSwapVector<CSphMatch> & dMatches );

private:
	// a simplified match (incoming matche don't have a static part)
	struct StoredMatch_t
	{
		CSphRowitem *	m_pDynamic = nullptr;
	};

	using StoredMatches_t = CSphVector<StoredMatch_t>;
	OpenHashTable_T<uint64_t, StoredMatches_t> m_hCache;

	uint64_t			m_uMaxSize = 0;
	uint64_t			m_uCurSize = 0;

	std::unique_ptr<ISphSchema> m_pSchema;
	uint64_t			CalcMatchMem ( const CSphMatch & tMatch );
};


MatchCache_c::MatchCache_c ( uint64_t uCacheSize )
	: m_uMaxSize ( uCacheSize )
{}


MatchCache_c::~MatchCache_c()
{
	int64_t iIterator = 0;
	std::pair<SphGroupKey_t, StoredMatches_t*> tRes;
	while ( ( tRes = m_hCache.Iterate ( iIterator ) ).second )
	{
		StoredMatches_t & dMatches = *tRes.second;
		for ( auto & i : dMatches )
		{
			CSphMatch tStub;
			tStub.m_pDynamic = i.m_pDynamic;
			m_pSchema->FreeDataPtrs(tStub);
			tStub.ResetDynamic();
		}
	}
}


void MatchCache_c::SetSchema ( const ISphSchema * pSchema )
{
	if ( m_pSchema )
		return;

	// keep a clone of the schema
	// we assume that the schema won't change during the lifetime of the cache
	m_pSchema = std::unique_ptr<ISphSchema> ( pSchema->CloneMe() );
}


uint64_t MatchCache_c::CalcMatchMem ( const CSphMatch & tMatch )
{
	uint64_t uMem = 0;
	for ( int i = 0; i < m_pSchema->GetAttrsCount(); i++ )
	{
		const auto & tAttr = m_pSchema->GetAttr(i);
		if ( !tAttr.IsDataPtr() )
			continue;

		const BYTE * pBlob = (const BYTE *)sphGetRowAttr ( tMatch.m_pDynamic, tAttr.m_tLocator );
		uMem += sphUnpackPtrAttr(pBlob).second;
	}

	uMem += m_pSchema->GetDynamicSize()*sizeof(CSphRowitem);
	uMem += sizeof(StoredMatch_t);
	return uMem;
}


bool MatchCache_c::Add ( uint64_t uHash, const CSphSwapVector<CSphMatch> & dMatches )
{
	if ( !m_pSchema )
		return false;

	if ( m_uCurSize>=m_uMaxSize )
		return false;

	StoredMatches_t dStoredMatches;

	for ( const auto & i : dMatches )
	{
		dStoredMatches.Add ( { i.m_pDynamic } );
		m_uCurSize += CalcMatchMem(i);
	}

	m_hCache.Add ( uHash, dStoredMatches );
	return true;
}


bool MatchCache_c::Fetch ( uint64_t uHash, CSphSwapVector<CSphMatch> & dMatches )
{
	StoredMatches_t * pMatches = m_hCache.Find(uHash);
	if ( !pMatches )
		return false;

	dMatches.Resize ( pMatches->GetLength() );
	for ( int i = 0; i < pMatches->GetLength(); i++ )
		dMatches[i].m_pDynamic = (*pMatches)[i].m_pDynamic;

	return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class MatchCalc_c : public MatchProcessor_i
{
public:
	MatchCalc_c ( ISphMatchSorter * pSorter ) : m_pSorter ( pSorter ) {}

	void Process ( CSphMatch * pMatch ) override	{ m_pSorter->Push ( *pMatch ); }
	bool ProcessInRowIdOrder() const override		{ return false; }
	void Process ( VecTraits_T<CSphMatch *> & dMatches ) override
	{
		for ( auto & i : dMatches )
			Process(i);
	}

protected:
	ISphMatchSorter * m_pSorter = nullptr;
};


class MatchCalcGrouped_c : public MatchCalc_c
{
public:
	MatchCalcGrouped_c ( ISphMatchSorter * pSorter ) : MatchCalc_c ( pSorter ) {}

	void Process ( CSphMatch * pMatch ) final
	{
		m_pSorter->PushGrouped ( *pMatch, m_bFirst );
		m_bFirst = false;
	}

private:
	bool m_bFirst = true;
};


class StoredFetch_c : public MatchProcessor_i
{
public:
			StoredFetch_c ( const ISphSchema & tSorterSchema, const CSphIndex & tRightIndex, uint64_t uNullMask );

	void	Process ( CSphMatch * pMatch ) override;
	bool	ProcessInRowIdOrder() const override	{ return false; }
	void	Process ( VecTraits_T<CSphMatch *> & dMatches ) override;

	bool	HasFieldToFetch() const					{ return !m_dFieldToFetch.IsEmpty(); }

private:
	const ISphSchema &					m_tSorterSchema;
	const CSphIndex &					m_tRightIndex;
	const CSphColumnInfo *				m_pId = nullptr;
	const CSphColumnInfo *				m_pNullMaskAttr = nullptr;
	std::unique_ptr<DocstoreSession_c>	m_pSession;
	int64_t								m_iSessionUID = 0;
	IntVec_t							m_dFieldToFetch;
	CSphVector<CSphAttrLocator>			m_dAttrRemap;
	uint64_t							m_uNullMask = 0;

	void	CreateDocstoreSession();
	void	SetupAttrRemap();
};


StoredFetch_c::StoredFetch_c ( const ISphSchema & tSorterSchema, const CSphIndex & tRightIndex, uint64_t uNullMask )
	: m_tSorterSchema ( tSorterSchema )
	, m_tRightIndex ( tRightIndex )
	, m_uNullMask ( uNullMask )
{
	CreateDocstoreSession();
	SetupAttrRemap();
}


void StoredFetch_c::Process ( CSphMatch * pMatch )
{
	assert ( !m_dFieldToFetch.IsEmpty() );
	assert(m_pId);

	SphAttr_t tDocId = pMatch->GetAttr ( m_pId->m_tLocator );

	// compare against a preset null mask that the join sorter uses
	bool bNull = m_pNullMaskAttr && pMatch->GetAttr ( m_pNullMaskAttr->m_tLocator )==m_uNullMask;

	DocstoreDoc_t tDoc;
	if ( !bNull && m_tRightIndex.GetDoc ( tDoc, tDocId, &m_dFieldToFetch, m_iSessionUID, true ) )
	{
		ARRAY_FOREACH ( i, tDoc.m_dFields )
		{
			const CSphAttrLocator & tLoc = m_dAttrRemap[i];
			auto pPrev = (BYTE*)pMatch->GetAttr(tLoc);
			SafeDeleteArray(pPrev);

			pMatch->SetAttr ( tLoc, (SphAttr_t)tDoc.m_dFields[i].LeakData() );
		}
	}
	else
	{
		ARRAY_FOREACH ( i, tDoc.m_dFields )
		{
			auto pPrev = (BYTE*)pMatch->GetAttr ( m_dAttrRemap[i] );
			SafeDeleteArray(pPrev);
		}
	}
}


void StoredFetch_c::Process ( VecTraits_T<CSphMatch *> & dMatches )
{
	for ( auto & i : dMatches )
		Process(i);
}


void StoredFetch_c::CreateDocstoreSession()
{
	m_pSession = std::make_unique<DocstoreSession_c>();
	m_iSessionUID = m_pSession->GetUID();

	// spawn buffered readers for the current session
	m_tRightIndex.CreateReader(m_iSessionUID);
}


void StoredFetch_c::SetupAttrRemap()
{
	CSphString sAttrName;
	sAttrName.SetSprintf ( "%s.%s", m_tRightIndex.GetName(), sphGetDocidName() );
	m_pId = m_tSorterSchema.GetAttr ( sAttrName.cstr() );
	assert(m_pId);

	const ISphSchema & tRightSchema = m_tRightIndex.GetMatchSchema();
	for ( int i = 0; i < tRightSchema.GetFieldsCount(); i++ )
	{
		const CSphColumnInfo & tField = tRightSchema.GetField(i);
		if ( !( tField.m_uFieldFlags & CSphColumnInfo::FIELD_STORED ) )
			continue;

		CSphString sAttrName;
		sAttrName.SetSprintf ( "%s.%s", m_tRightIndex.GetName(), tField.m_sName.cstr() );

		int iDocStoreFieldId = m_tRightIndex.GetFieldId ( tField.m_sName, DOCSTORE_TEXT );
		int iAttrId = m_tSorterSchema.GetAttrIndex ( sAttrName.cstr() );
		assert ( iDocStoreFieldId!=-1 && iAttrId!=-1 );

		m_dFieldToFetch.Add(iDocStoreFieldId);
		m_dAttrRemap.Add ( m_tSorterSchema.GetAttr(iAttrId).m_tLocator );
	}

	m_pNullMaskAttr = m_tSorterSchema.GetAttr ( GetNullMaskAttrName() );
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class JoinSorter_c : public ISphMatchSorter
{
public:
				JoinSorter_c ( const CSphIndex * pIndex, const CSphIndex * pJoinedIndex, const CSphQuery & tQuery, ISphMatchSorter * pSorter, bool bJoinedGroupSort );

	bool		IsGroupby() const override											{ return m_pSorter->IsGroupby(); }
	void		SetState ( const CSphMatchComparatorState & tState ) override		{ m_pSorter->SetState(tState); }
	const		CSphMatchComparatorState & GetState() const override				{ return m_pSorter->GetState(); }
	void		SetGroupState ( const CSphMatchComparatorState & tState ) override	{ m_pSorter->SetGroupState(tState); }
	void		SetBlobPool ( const BYTE * pBlobPool ) override;
	void		SetColumnar ( columnar::Columnar_i * pColumnar ) override			{ m_pSorter->SetColumnar(pColumnar); }
	void		SetSchema ( ISphSchema * pSchema, bool bRemapCmp ) override;
	const ISphSchema *	GetSchema() const override									{ return m_pSorter->GetSchema(); }
	bool		Push ( const CSphMatch & tEntry ) override							{ return Push_T ( tEntry, [this]( const CSphMatch & tMatch ){ return m_pSorter->Push(tMatch); } ); }
	void		Push ( const VecTraits_T<const CSphMatch> & dMatches ) override;
	bool		PushGrouped ( const CSphMatch & tEntry, bool bNewSet ) override		{ return Push_T ( tEntry, [this,bNewSet]( const CSphMatch & tMatch ){ return m_pSorter->PushGrouped ( tMatch, bNewSet ); } ); }
	int			GetLength() override												{ return m_pSorter->GetLength(); }
	int64_t		GetTotalCount() const override										{ return m_pSorter->GetTotalCount(); }
	void		Finalize ( MatchProcessor_i & tProcessor, bool bCallProcessInResultSetOrder, bool bFinalizeMatches ) override { m_pSorter->Finalize ( tProcessor, bCallProcessInResultSetOrder, bFinalizeMatches ); }
	int			Flatten ( CSphMatch * pTo ) override								{ return m_pSorter->Flatten(pTo); }
	const CSphMatch * GetWorst() const override										{ return m_pSorter->GetWorst(); }
	bool		CanBeCloned() const override										{ return m_pSorter->CanBeCloned(); }
	ISphMatchSorter * Clone() const override										{ return new JoinSorter_c ( m_pIndex, m_pJoinedIndex, m_tQuery, m_pSorter->Clone(), !m_bFinalCalcOnly ); }
	void		MoveTo ( ISphMatchSorter * pRhs, bool bCopyMeta ) override			{ m_pSorter->MoveTo ( ((JoinSorter_c *)pRhs)->m_pSorter.get(), bCopyMeta ); }
	void		CloneTo ( ISphMatchSorter * pTrg ) const override					{ m_pSorter->CloneTo(pTrg); }
	void		SetFilteredAttrs ( const sph::StringSet & hAttrs, bool bAddDocid ) override	{ m_pSorter->SetFilteredAttrs(hAttrs, bAddDocid); }
	void		TransformPooled2StandalonePtrs ( GetBlobPoolFromMatch_fn fnBlobPoolFromMatch, GetColumnarFromMatch_fn fnGetColumnarFromMatch, bool bFinalizeSorters ) override { m_pSorter->TransformPooled2StandalonePtrs(fnBlobPoolFromMatch, fnGetColumnarFromMatch, bFinalizeSorters); }
	void		SetRandom ( bool bRandom ) override									{ m_pSorter->SetRandom(bRandom); }
	bool		IsRandom() const override											{ return m_pSorter->IsRandom(); }
	int			GetMatchCapacity() const override									{ return m_pSorter->GetMatchCapacity(); }
	RowTagged_t	GetJustPushed() const override										{ return m_pSorter->GetJustPushed(); }
	VecTraits_T<RowTagged_t> GetJustPopped() const override							{ return m_pSorter->GetJustPopped(); }
	bool		IsCutoffDisabled() const override									{ return m_pSorter->IsCutoffDisabled(); }
	void		SetMerge ( bool bMerge ) override									{ m_pSorter->SetMerge(bMerge); }
	bool		IsPrecalc() const override											{ return false; }
	bool		IsJoin() const override												{ return true; }
	void		FinalizeJoin ( CSphString & sWarning ) override;

	bool		GetErrorFlag() const												{ return m_bErrorFlag; }
	const CSphString & GetErrorMessage() const										{ return m_sErrorMessage; }

protected:
	template <typename PUSH>
	FORCE_INLINE bool Push_T ( const CSphMatch & tMatch, PUSH && fnPush );

private:
	struct JoinAttrRemap_t
	{
		CSphAttrLocator			m_tLocSrc;
		CSphAttrLocator			m_tLocDst;
		bool					m_bJsonRepack = false;
	};

	struct FilterRemap_t
	{
		int				m_iFilterId = -1;
		CSphAttrLocator	m_tLocator;
		bool			m_bBlob = false;
	};

	CSphQuery						m_tJoinQuery;
	std::unique_ptr<QueryParser_i>	m_pJoinQueryParser;
	const CSphIndex *				m_pIndex = nullptr;
	const CSphIndex *				m_pJoinedIndex = nullptr;
	const CSphQuery					m_tQuery;
	CSphMatch						m_tMatch;
	std::unique_ptr<ISphMatchSorter> m_pSorter;
	std::unique_ptr<ISphMatchSorter> m_pRightSorter;
	std::unique_ptr<ISphSchema>		m_pRightSorterSchema;
	const BYTE *					m_pBlobPool = nullptr;
	const CSphColumnInfo *			m_pAttrNullBitmask = nullptr;
	CSphSwapVector<CSphMatch>		m_dMatches;
	SmallStringHash_T<CSphString>	m_hAttrRemap;
	CSphVector<JoinAttrRemap_t>		m_dJoinRemap;
	bool							m_bNeedToSetupRemap = true;
	CSphVector<FilterRemap_t>		m_dFilterRemap;
	int								m_iDynamicSize = 0;
	bool							m_bFinalCalcOnly = false;
	const ISphSchema *				m_pSorterSchema = nullptr;
	CSphVector<ContextCalcItem_t>	m_dAggregates;

	MatchCache_c					m_tCache;
	bool							m_bCacheOk = true;

	std::unique_ptr<BYTE[]>			m_pNullMask;
	uint64_t						m_uNullMask = 0;

	bool							m_bErrorFlag = false;
	CSphString						m_sErrorMessage;

	bool		SetupJoinQuery ( int iDynamicSize, CSphString & sError );
	bool		SetupJoinSorter ( CSphString & sError );
	void		SetupJoinAttrRemap();
	void		SetupSorterSchema();
	void		SetupNullMask();
	void		SetupAggregates();
	FORCE_INLINE uint64_t SetupJoinFilters ( const CSphMatch & tEntry );
	bool		SetupRightFilters ( CSphString & sError );
	bool		SetupOnFilters ( CSphString & sError );
	void		AddToJoinSelectList ( const CSphString & sExpr, const CSphString & sAlias );
	void		SetupJoinSelectList();
	void		RepackJsonFieldAsStr ( const CSphMatch & tSrcMatch, const CSphAttrLocator & tLocSrc, const CSphAttrLocator & tLocDst );
	void		ProduceCacheSizeWarning ( CSphString & sWarning );
	void		PopulateStoredFields();
};


JoinSorter_c::JoinSorter_c ( const CSphIndex * pIndex, const CSphIndex * pJoinedIndex, const CSphQuery & tQuery, ISphMatchSorter * pSorter, bool bJoinedGroupSort )
	: m_pIndex ( pIndex )
	, m_pJoinedIndex ( pJoinedIndex )
	, m_tQuery ( tQuery )
	, m_pSorter ( pSorter )
	, m_tCache ( GetJoinCacheSize() )
{
	assert ( pIndex && pJoinedIndex );

	m_bFinalCalcOnly = !bJoinedGroupSort && tQuery.m_eJoinType==JoinType_e::LEFT;
	m_bErrorFlag = !SetupJoinQuery ( m_pSorter->GetSchema()->GetDynamicSize(), m_sErrorMessage );
}


void JoinSorter_c::SetBlobPool ( const BYTE * pBlobPool )
{
	m_pBlobPool = pBlobPool;
	m_pSorter->SetBlobPool(pBlobPool);
}


void JoinSorter_c::SetSchema ( ISphSchema * pSchema, bool bRemapCmp )
{
	m_pSorter->SetSchema ( pSchema, bRemapCmp );
	m_bErrorFlag = !SetupJoinQuery ( pSchema->GetDynamicSize(), m_sErrorMessage );
}


void JoinSorter_c::SetupSorterSchema()
{
	m_pSorterSchema = m_pSorter->GetSchema();
	assert ( m_pSorterSchema );
	m_pAttrNullBitmask = m_pSorterSchema->GetAttr ( GetNullMaskAttrName() );
}


void JoinSorter_c::SetupNullMask()
{
	if ( !m_pAttrNullBitmask )
		return;

	if ( m_pAttrNullBitmask->m_eAttrType==SPH_ATTR_STRINGPTR )
	{
		int iNumJoinAttrs = 0;
		int iDynamic = 0;
		for ( int i = 0; i < m_pSorterSchema->GetAttrsCount(); i++ )
		{
			const auto & tAttr = m_pSorterSchema->GetAttr(i);
			if ( !tAttr.m_tLocator.m_bDynamic )
				continue;

			iDynamic++;
			if ( tAttr.IsJoined() )
				iNumJoinAttrs = Max ( iNumJoinAttrs, iDynamic );
		}

		BitVec_T<BYTE> tMask(iNumJoinAttrs);

		iDynamic = 0;
		for ( int i = 0; i < m_pSorterSchema->GetAttrsCount(); i++ )
		{
			const auto & tAttr = m_pSorterSchema->GetAttr(i);
			if ( !tAttr.m_tLocator.m_bDynamic )
				continue;

			if ( tAttr.IsJoined() )
				tMask.BitSet(iDynamic);

			iDynamic++;
		}

		m_pNullMask = std::unique_ptr<BYTE[]>( sphPackPtrAttr ( { tMask.Begin(), tMask.GetSizeBytes() } ) );
		m_uNullMask = (uint64_t)m_pNullMask.get();
		return;
	}

	// we keep null flags only for attributes with a dynamic locator
	// and these attributes need to be from the right table
	m_uNullMask = 0;
	int iDynamic = 0;
	for ( int i = 0; i < m_pSorterSchema->GetAttrsCount(); i++ )
	{
		const auto & tAttr = m_pSorterSchema->GetAttr(i);
		if ( !tAttr.m_tLocator.m_bDynamic )
			continue;

		if ( tAttr.IsJoined() )
			m_uNullMask |= 1ULL << iDynamic;

		iDynamic++;
	}
}


void JoinSorter_c::SetupAggregates()
{
	for ( int i = 0; i < m_pSorterSchema->GetAttrsCount(); i++ )
	{
		const auto & tAttr = m_pSorterSchema->GetAttr(i);
		if ( tAttr.m_eAggrFunc!=SPH_AGGR_NONE && tAttr.m_eStage==SPH_EVAL_SORTER && GetJoinAttrName ( tAttr.m_sName, CSphString ( m_pJoinedIndex->GetName() ) ) )
			m_dAggregates.Add ( { tAttr.m_tLocator, tAttr.m_eAttrType, tAttr.m_pExpr } );
	}
}


bool JoinSorter_c::SetupJoinQuery ( int iDynamicSize, CSphString & sError )
{
	m_pJoinQueryParser = sphCreatePlainQueryParser();
	m_tJoinQuery.m_pQueryParser = m_pJoinQueryParser.get();
	m_tJoinQuery.m_iLimit = DEFAULT_MAX_MATCHES;
	m_tJoinQuery.m_iCutoff = 0;
	m_tJoinQuery.m_sQuery = m_tJoinQuery.m_sRawQuery = m_tQuery.m_sJoinQuery;

	m_tMatch.Reset ( iDynamicSize );
	SetupJoinSelectList();
	SetupSorterSchema();
	if ( !SetupRightFilters(sError) )	return false;
	if ( !SetupOnFilters(sError) )		return false;
	if ( !SetupJoinSorter(sError) )		return false;

	SetupNullMask();
	SetupAggregates();
	m_iDynamicSize = iDynamicSize;

	return true;
}


bool JoinSorter_c::SetupJoinSorter ( CSphString & sError )
{
	SphQueueSettings_t tQueueSettings ( m_pJoinedIndex->GetMatchSchema() );
	tQueueSettings.m_bComputeItems = true;
	SphQueueRes_t tRes;
	m_pRightSorter = std::unique_ptr<ISphMatchSorter> ( sphCreateQueue ( tQueueSettings, m_tJoinQuery, sError, tRes ) );
	if ( !m_pRightSorter )
		return false;

	m_pRightSorterSchema = std::unique_ptr<ISphSchema> ( m_pRightSorter->GetSchema()->CloneMe() );
	assert(m_pRightSorterSchema);

	return true;
}


void JoinSorter_c::SetupJoinAttrRemap()
{
	m_dJoinRemap.Resize(0);

	auto * pSorterSchema = m_pSorter->GetSchema();
	auto * pJoinSorterSchema = m_pRightSorter->GetSchema();
	for ( int i = 0; i < pJoinSorterSchema->GetAttrsCount(); i++ )
	{
		auto & tAttrSrc = pJoinSorterSchema->GetAttr(i);
		if ( !m_hAttrRemap.Exists ( tAttrSrc.m_sName ) )
			continue;

		const CSphString & sDstAttr = m_hAttrRemap[tAttrSrc.m_sName];
		auto * pAttrDst = pSorterSchema->GetAttr ( sDstAttr.cstr() );
		assert(pAttrDst);
		bool bJsonRepack = pAttrDst->m_sName.Begins ( GetInternalJsonPrefix() ) || pAttrDst->m_sName.Begins ( GetInternalAttrPrefix() );
		m_dJoinRemap.Add ( { tAttrSrc.m_tLocator, pAttrDst->m_tLocator, bJsonRepack } );
	}

	m_bNeedToSetupRemap = false;
}


void JoinSorter_c::RepackJsonFieldAsStr ( const CSphMatch & tSrcMatch, const CSphAttrLocator & tLocSrc, const CSphAttrLocator & tLocDst )
{
	auto pValue = (BYTE *)m_tMatch.GetAttr(tLocDst);
	SafeDeleteArray(pValue);

	SphAttr_t tJsonFieldPtr = tSrcMatch.GetAttr(tLocSrc);
	if ( !tJsonFieldPtr )
		return;

	const BYTE * pVal = (BYTE *)tJsonFieldPtr;
	auto tBlob = sphUnpackPtrAttr(pVal);
	pVal = tBlob.first;
	ESphJsonType eJson = (ESphJsonType)*pVal++;
	CSphString sResult = FormatJsonAsSortStr ( pVal, eJson );
	int iStrLen = sResult.Length();
	BYTE * pData = nullptr;
	SphAttr_t uValue = (SphAttr_t) sphPackPtrAttr ( iStrLen+1, &pData );
	memcpy ( pData, sResult.cstr(), iStrLen+1 );

	m_tMatch.SetAttr ( tLocDst, uValue );
}

template <typename PUSH>
bool JoinSorter_c::Push_T ( const CSphMatch & tEntry, PUSH && fnPush )
{
	if ( m_bFinalCalcOnly )
		return fnPush(tEntry);

	bool bInCache = true;
	uint64_t uJoinOnFilterHash = SetupJoinFilters(tEntry);
	if ( !m_tCache.Fetch ( uJoinOnFilterHash, m_dMatches ) )
	{
		CSphQueryResultMeta tMeta;
		CSphQueryResult tQueryResult;
		tQueryResult.m_pMeta = &tMeta;

		// restore non-standalone schema
		// FIXME!!!! make a SetSchema that does not take ownership of the schema
		m_pRightSorter->SetSchema ( m_pRightSorterSchema->CloneMe(), true );

		CSphMultiQueryArgs tArgs(1);
		ISphMatchSorter * pSorter = m_pRightSorter.get();
		m_pJoinedIndex->MultiQuery ( tQueryResult, m_tJoinQuery, { &pSorter, 1 }, tArgs );
		m_dMatches.Resize(0);

		// setup join attr remap, but do it only once
		// we can't do that before because we need to remap from the standalone schema and we get it only after the first query
		if ( m_bNeedToSetupRemap )
			SetupJoinAttrRemap();

		if ( pSorter->GetLength() )
		{
			int iCopied = pSorter->Flatten ( m_dMatches.AddN ( pSorter->GetLength() ) );
			m_dMatches.Resize(iCopied);
		}

		m_tCache.SetSchema ( pSorter->GetSchema() );
		bInCache = m_tCache.Add ( uJoinOnFilterHash, m_dMatches );
		m_bCacheOk &= bInCache;
	}

	CSphRowitem * pDynamic = m_tMatch.m_pDynamic;
	memcpy ( &m_tMatch, &tEntry, sizeof(m_tMatch) );
	m_tMatch.m_pDynamic = pDynamic;

	bool bAnythingPushed = false;
	ARRAY_FOREACH ( iMatch, m_dMatches )
	{
		memcpy ( m_tMatch.m_pDynamic, tEntry.m_pDynamic, m_iDynamicSize*sizeof(CSphRowitem) );
		auto & tMatchFromRset = m_dMatches[iMatch];

		for ( auto & i : m_dJoinRemap )
		{
			if ( i.m_bJsonRepack )
				RepackJsonFieldAsStr ( tMatchFromRset, i.m_tLocSrc, i.m_tLocDst );
			else
				m_tMatch.SetAttr ( i.m_tLocDst, tMatchFromRset.GetAttr ( i.m_tLocSrc ) );
		}

		CalcContextItems ( m_tMatch, m_dAggregates );
		bAnythingPushed |= fnPush(m_tMatch);

		// clear repacked json
		for ( auto & i : m_dJoinRemap )
			if ( i.m_bJsonRepack )
			{
				auto pValue = (BYTE *)m_tMatch.GetAttr(i.m_tLocDst);
				SafeDeleteArray(pValue);
			}
	}

	if ( bInCache )
	{
		for ( auto & i : m_dMatches )
			i.m_pDynamic = nullptr;
	}
	else
	{
		for ( auto & i : m_dMatches )
		{
			m_pRightSorterSchema->FreeDataPtrs(i);
			i.ResetDynamic();
		}
	}

	if ( !m_dMatches.GetLength() && m_tQuery.m_eJoinType==JoinType_e::LEFT )
	{
		memcpy ( m_tMatch.m_pDynamic, tEntry.m_pDynamic, m_iDynamicSize*sizeof(CSphRowitem) );
		CalcContextItems ( m_tMatch, m_dAggregates );

		// set NULL bitmask
		assert(m_pAttrNullBitmask);
		m_tMatch.SetAttr ( m_pAttrNullBitmask->m_tLocator, m_uNullMask );
		return fnPush(m_tMatch);
	}

	return bAnythingPushed;
}


void JoinSorter_c::Push ( const VecTraits_T<const CSphMatch> & dMatches )
{
	for ( auto & i : dMatches )
		Push(i);
}


void JoinSorter_c::ProduceCacheSizeWarning ( CSphString & sWarning )
{
	if ( !m_bCacheOk )
		sWarning.SetSprintf ( "Join cache overflow detected; increase join_cache_size to improve performance" );
}


void JoinSorter_c::PopulateStoredFields()
{
	StoredFetch_c tCalc ( *m_pSorterSchema, *m_pJoinedIndex, m_uNullMask );
	if ( tCalc.HasFieldToFetch() )
		m_pSorter->Finalize ( tCalc, false, false );
}


void JoinSorter_c::FinalizeJoin ( CSphString & sWarning )
{
	if ( !m_bFinalCalcOnly )
	{
		PopulateStoredFields();
		ProduceCacheSizeWarning(sWarning);
		return;
	}

	// replace underlying sorter with a new one
	// and fill it with matches that we already have
	std::unique_ptr<ISphMatchSorter> pOldSorter = std::move(m_pSorter);

	m_pSorter = std::unique_ptr<ISphMatchSorter> ( pOldSorter->Clone() );
	SetupSorterSchema();

	m_bFinalCalcOnly = false;
	if ( pOldSorter->IsGroupby() )
	{
		MatchCalcGrouped_c tCalc(this);
		pOldSorter->Finalize ( tCalc, false, false );
	}
	else
	{
		MatchCalc_c tCalc(this);
		pOldSorter->Finalize ( tCalc, false, false );
	}

	PopulateStoredFields();
	ProduceCacheSizeWarning(sWarning);
}


bool JoinSorter_c::SetupRightFilters ( CSphString & sError )
{
	m_tJoinQuery.m_dFilters.Resize(0);

	// add rhs filters that we removed in TransformFilters
	CSphString sPrefix;
	sPrefix.SetSprintf ( "%s.", m_pJoinedIndex->GetName() );

	CSphVector<std::pair<int,bool>> dRightFilters;
	ARRAY_FOREACH ( i, m_tQuery.m_dFilters )
	{
		const auto & tFilter = m_tQuery.m_dFilters[i];
		bool bHasPrefix = tFilter.m_sAttrName.Begins ( sPrefix.cstr() );
		const CSphColumnInfo * pFilterAttr = m_pSorterSchema->GetAttr ( tFilter.m_sAttrName.cstr() );
		if ( pFilterAttr )
		{
			if ( !pFilterAttr->IsJoined() )
				continue;
		}
		else
		{
			if ( !bHasPrefix )
				continue;
		}

		dRightFilters.Add ( {i,bHasPrefix} );
	}

	if ( m_tQuery.m_dFilterTree.GetLength() )
	{
		if ( !dRightFilters.GetLength() )
			return true;

		if ( dRightFilters.GetLength()==m_tQuery.m_dFilters.GetLength() )
			m_tJoinQuery.m_dFilterTree = m_tQuery.m_dFilterTree;
		else
		{
			sError = "Mixed filters on left/right tables with OR between them are not currently supported in JOIN";
			return false;
		}
	}

	ARRAY_FOREACH ( i, dRightFilters )
	{
		const auto & tFilter = m_tQuery.m_dFilters[dRightFilters[i].first];
		m_tJoinQuery.m_dFilters.Add(tFilter);

		// remove table name prefix
		if ( dRightFilters[i].second )
		{
			int iPrefixLen = sPrefix.Length();
			m_tJoinQuery.m_dFilters.Last().m_sAttrName = tFilter.m_sAttrName.SubString ( iPrefixLen, tFilter.m_sAttrName.Length() - iPrefixLen );
		}
	}

	return true;
}


bool JoinSorter_c::SetupOnFilters ( CSphString & sError )
{
	for ( auto & tOnFilter : m_tQuery.m_dOnFilters )
	{
		CSphFilterSettings & tFilter = m_tJoinQuery.m_dFilters.Add();
		tFilter.m_dValues.Resize(0);
		tFilter.m_dStrings.Resize(0);

		CSphString sAttrIdx1 = tOnFilter.m_sAttr1;
		CSphString sAttrIdx2 = tOnFilter.m_sAttr2;
		CSphString sIdx1 = tOnFilter.m_sIdx1;
		CSphString sIdx2 = tOnFilter.m_sIdx2;

		if ( tOnFilter.m_sIdx1==m_pJoinedIndex->GetName() )
		{
			assert ( tOnFilter.m_sIdx2==m_pIndex->GetName() );
			Swap ( sAttrIdx1, sAttrIdx2 );
			Swap ( sIdx1, sIdx2 );
		}

		// FIXME! handle compound names for left table (e.g. 'table1.id')
		const CSphColumnInfo * pAttr1 = m_pSorter->GetSchema()->GetAttr ( sAttrIdx1.cstr() );
		assert(pAttr1);

		// maybe it is a stored field?
		if ( pAttr1->m_eAttrType==SPH_ATTR_STRINGPTR && pAttr1->m_eStage==SPH_EVAL_POSTLIMIT )
		{
			sError.SetSprintf ( "Unable to perform join on a stored field '%s.%s'", sIdx1.cstr(), pAttr1->m_sName.cstr() );
			return false;
		}

		const CSphColumnInfo * pAttr2 = m_pJoinedIndex->GetMatchSchema().GetAttr ( sAttrIdx2.cstr() );
		if ( pAttr2 && pAttr2->m_eAttrType==SPH_ATTR_STRINGPTR && pAttr2->m_eStage==SPH_EVAL_POSTLIMIT )
		{
			sError.SetSprintf ( "Unable to perform join on a stored field '%s.%s'", sIdx2.cstr(), pAttr2->m_sName.cstr() );
			return false;
		}

		bool bStringFilter = pAttr1->m_eAttrType==SPH_ATTR_STRING;

		tFilter.m_sAttrName = sAttrIdx2;
		tFilter.m_eType		= bStringFilter ? SPH_FILTER_STRING : SPH_FILTER_VALUES;

		m_dFilterRemap.Add ( { m_tJoinQuery.m_dFilters.GetLength()-1, pAttr1->m_tLocator, bStringFilter } );

		if ( bStringFilter )
			tFilter.m_dStrings.Resize(1);
		else
			tFilter.m_dValues.Resize(1);
	}

	return true;
}


uint64_t JoinSorter_c::SetupJoinFilters ( const CSphMatch & tEntry )
{
	uint64_t uHash = 0;
	ARRAY_FOREACH ( i, m_dFilterRemap )
	{
		const auto & tRemap = m_dFilterRemap[i];
		auto & tFilter = m_tJoinQuery.m_dFilters[tRemap.m_iFilterId];
		if ( tRemap.m_bBlob )
		{
			ByteBlob_t tBlob = tEntry.FetchAttrData ( tRemap.m_tLocator, m_pBlobPool );
			tFilter.m_dStrings[0] = CSphString ( (const char*)tBlob.first, tBlob.second );

			uHash = HashWithSeed ( tBlob.first, tBlob.second, uHash );
		}
		else
		{
			SphAttr_t tValue = tEntry.GetAttr ( tRemap.m_tLocator );
			tFilter.m_dValues[0] = tValue;

			uHash = HashWithSeed ( &tValue, sizeof(tValue), uHash );
		}
	}

	return uHash;
}


void JoinSorter_c::AddToJoinSelectList ( const CSphString & sExpr, const CSphString & sAlias )
{
	if ( sExpr=="*" || sAlias=="*" )
		return;

	const ISphSchema * pSorterSchema = m_pSorter->GetSchema();
	assert(pSorterSchema);

	CSphString sJoinExpr;
	if ( !GetJoinAttrName ( sExpr, CSphString ( m_pJoinedIndex->GetName() ), &sJoinExpr ) )
		return;

	int iSorterAttrId = pSorterSchema->GetAttrIndex ( sExpr.cstr() );
	if ( iSorterAttrId==-1 )
		iSorterAttrId = pSorterSchema->GetAttrIndex ( sAlias.cstr() );

	if ( iSorterAttrId==-1 )
	{
		// maybe it's a JSON attr?
		if ( !sphJsonNameSplit ( sExpr.cstr(), m_pJoinedIndex->GetName() ) )
			return;

		// try remapped groupby json attr
		CSphString sRemapped;
		sRemapped.SetSprintf ( "%s_%s", GetInternalJsonPrefix(), sExpr.cstr() );
		iSorterAttrId = pSorterSchema->GetAttrIndex ( sRemapped.cstr() );

		if ( iSorterAttrId==-1 )
		{
			// try remapped sort attr
			sRemapped.SetSprintf ( "%s%s", GetInternalAttrPrefix(), sExpr.cstr() );
			iSorterAttrId = pSorterSchema->GetAttrIndex ( sRemapped.cstr() );
		}		
	}

	assert ( iSorterAttrId!=-1 );
	CSphString sJoinAlias = sExpr==sAlias ? sJoinExpr : sAlias;

	// don't add duplicates
	for ( const auto & i : m_tJoinQuery.m_dItems )
		if ( i.m_sExpr==sJoinExpr && i.m_sAlias==sJoinAlias )
			return;

	m_hAttrRemap.Add ( pSorterSchema->GetAttr(iSorterAttrId).m_sName, sJoinAlias );

	auto & tItem = m_tJoinQuery.m_dItems.Add();
	tItem.m_sExpr = sJoinExpr;
	tItem.m_sAlias = sJoinAlias;
}


void JoinSorter_c::SetupJoinSelectList()
{
	m_tJoinQuery.m_dItems.Resize(0);
	m_hAttrRemap.Reset();

	bool bHaveStar = m_tQuery.m_dItems.any_of ( []( const CSphQueryItem & tItem ) { return tItem.m_sExpr=="*" || tItem.m_sAlias=="*"; } );
	if ( bHaveStar )
	{
	 	const CSphSchema & tJoinedSchema = m_pJoinedIndex->GetMatchSchema();
		for ( int i = 0; i < tJoinedSchema.GetAttrsCount(); i++ )
		{
			auto & tAttr = tJoinedSchema.GetAttr(i);
			if ( sphIsInternalAttr(tAttr) )
				continue;

			CSphString sAttrName;
			sAttrName.SetSprintf ( "%s.%s", m_pJoinedIndex->GetName(), tAttr.m_sName.cstr() );
			AddToJoinSelectList ( sAttrName, sAttrName );
		}
	}

	for ( const auto & i : m_tQuery.m_dItems )
		AddToJoinSelectList ( i.m_sExpr, i.m_sAlias );

	if ( !m_tQuery.m_sGroupBy.IsEmpty() )
	{
		StrVec_t dGroupby = ParseGroupBy ( m_tQuery.m_sGroupBy );
		for ( const auto & i : dGroupby )
			AddToJoinSelectList ( i, i );
	}

	// find remapped sorting attrs
	auto * pSorterSchema = m_pSorter->GetSchema();
	assert(pSorterSchema);
	for ( int i = 0; i < pSorterSchema->GetAttrsCount(); i++ )
	{
		auto & tAttr = pSorterSchema->GetAttr(i);
		if ( sphIsInternalAttr(tAttr) || !IsSortStringInternal ( tAttr.m_sName.cstr() ) )
			continue;

		int iPrefixLen = strlen ( GetInternalAttrPrefix() );
		CSphString sJoinedAttrName = tAttr.m_sName.cstr()+iPrefixLen;
		AddToJoinSelectList ( sJoinedAttrName, sJoinedAttrName );
	}

	// fetch docid; we need it for docstore queries
	CSphString sId;
	sId.SetSprintf ( "%s.%s", m_pJoinedIndex->GetName(), sphGetDocidName() );
	AddToJoinSelectList ( sId, sId );
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class JoinMultiSorter_c : public JoinSorter_c
{
public:
			JoinMultiSorter_c ( const CSphIndex * pIndex, const CSphIndex * pJoinedIndex, const CSphQuery & tQuery, VecTraits_T<ISphMatchSorter *> dSorters, bool bJoinedGroupSort );

	bool	Push ( const CSphMatch & tEntry ) override;
	bool	PushGrouped ( const CSphMatch & tEntry, bool bNewSet ) override;

	// cloning multi-sorter is hard with the current architecture
	// as it holds pointers to sorters that also exist in dSorters array in matching/fullscan
	// for cloning to work we would need to clone the sorters that we hold and also somehow sync them with dSorters
	bool	CanBeCloned() const override { return false; }

private:
	CSphVector<ISphMatchSorter *> m_dSorters;	// we don't own 1..N sorters (JoinSorter_c owns sorter #0)
};


JoinMultiSorter_c::JoinMultiSorter_c ( const CSphIndex * pIndex, const CSphIndex * pJoinedIndex, const CSphQuery & tQuery, VecTraits_T<ISphMatchSorter *> dSorters, bool bJoinedGroupSort )
	: JoinSorter_c ( pIndex, pJoinedIndex, tQuery, dSorters[0], bJoinedGroupSort )
{
	m_dSorters.Resize ( dSorters.GetLength() );
	memcpy ( m_dSorters.Begin(), dSorters.Begin(), dSorters.GetLengthBytes() );
}


bool JoinMultiSorter_c::Push ( const CSphMatch & tEntry )
{
	return Push_T ( tEntry, [this]( const CSphMatch & tMatch )
		{
			bool bNew = false;
			for ( auto & i : m_dSorters )
				bNew |= i->Push(tMatch);

			return bNew;
		}
	);
}


bool JoinMultiSorter_c::PushGrouped ( const CSphMatch & tEntry, bool bNewSet )
{
	return Push_T ( tEntry, [this,bNewSet]( const CSphMatch & tMatch )
		{
			bool bNew = false;
			for ( auto & i : m_dSorters )
				bNew |= i->PushGrouped ( tMatch, bNewSet );

			return bNew;
		} );
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class SorterWrapperNoPush_c : public ISphMatchSorter
{
public:
				SorterWrapperNoPush_c ( ISphMatchSorter * pSorter ) : m_pSorter ( pSorter ) {}

	bool		IsGroupby() const override											{ return m_pSorter->IsGroupby(); }
	void		SetState ( const CSphMatchComparatorState & tState ) override		{ m_pSorter->SetState(tState); }
	const		CSphMatchComparatorState & GetState() const override				{ return m_pSorter->GetState(); }
	void		SetGroupState ( const CSphMatchComparatorState & tState ) override	{ m_pSorter->SetGroupState(tState); }
	void		SetBlobPool ( const BYTE * pBlobPool ) override						{ m_pSorter->SetBlobPool(pBlobPool); }
	void		SetColumnar ( columnar::Columnar_i * pColumnar ) override			{ m_pSorter->SetColumnar(pColumnar); }
	void		SetSchema ( ISphSchema * pSchema, bool bRemapCmp ) override			{ m_pSorter->SetSchema(pSchema, bRemapCmp); }
	const ISphSchema *	GetSchema() const override									{ return m_pSorter->GetSchema(); }
	bool		Push ( const CSphMatch & tEntry ) override							{ return false; }
	void		Push ( const VecTraits_T<const CSphMatch> & dMatches ) override		{}
	bool		PushGrouped ( const CSphMatch & tEntry, bool bNewSet ) override		{ return false; }
	int			GetLength() override												{ return m_pSorter->GetLength(); }
	int64_t		GetTotalCount() const override										{ return m_pSorter->GetTotalCount(); }
	void		Finalize ( MatchProcessor_i & tProcessor, bool bCallProcessInResultSetOrder, bool bFinalizeMatches ) override { m_pSorter->Finalize ( tProcessor, bCallProcessInResultSetOrder, bFinalizeMatches ); }
	int			Flatten ( CSphMatch * pTo ) override								{ return m_pSorter->Flatten(pTo); }
	const CSphMatch * GetWorst() const override										{ return m_pSorter->GetWorst(); }
	bool		CanBeCloned() const override										{ return m_pSorter->CanBeCloned(); }
	ISphMatchSorter * Clone() const override										{ return new SorterWrapperNoPush_c ( m_pSorter->Clone() ); }
	void		MoveTo ( ISphMatchSorter * pRhs, bool bCopyMeta ) override			{ m_pSorter->MoveTo ( ((SorterWrapperNoPush_c *)pRhs)->m_pSorter.get(), bCopyMeta ); }
	void		CloneTo ( ISphMatchSorter * pTrg ) const override					{ m_pSorter->CloneTo(pTrg); }
	void		SetFilteredAttrs ( const sph::StringSet & hAttrs, bool bAddDocid ) override	{ m_pSorter->SetFilteredAttrs(hAttrs, bAddDocid); }
	void		TransformPooled2StandalonePtrs ( GetBlobPoolFromMatch_fn fnBlobPoolFromMatch, GetColumnarFromMatch_fn fnGetColumnarFromMatch, bool bFinalizeSorters ) override { m_pSorter->TransformPooled2StandalonePtrs(fnBlobPoolFromMatch, fnGetColumnarFromMatch, bFinalizeSorters); }
	void		SetRandom ( bool bRandom ) override									{ m_pSorter->SetRandom(bRandom); }
	bool		IsRandom() const override											{ return m_pSorter->IsRandom(); }
	int			GetMatchCapacity() const override									{ return m_pSorter->GetMatchCapacity(); }
	RowTagged_t	GetJustPushed() const override										{ return m_pSorter->GetJustPushed(); }
	VecTraits_T<RowTagged_t> GetJustPopped() const override							{ return m_pSorter->GetJustPopped(); }
	bool		IsCutoffDisabled() const override									{ return m_pSorter->IsCutoffDisabled(); }
	void		SetMerge ( bool bMerge ) override									{ m_pSorter->SetMerge(bMerge); }
	bool		IsPrecalc() const override											{ return m_pSorter->IsPrecalc(); }

private:
	std::unique_ptr<ISphMatchSorter> m_pSorter;
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool CheckJoinOnFilters ( const CSphIndex * pIndex, const CSphIndex * pJoinedIndex, const CSphQuery & tQuery, CSphString & sError )
{
	if ( !tQuery.m_dOnFilters.GetLength() )
	{
		sError.SetSprintf ( "JOIN ON condition is empty" );
		return false;
	}

	for ( const auto & i : tQuery.m_dOnFilters )
	{
		if ( i.m_sIdx1!=pIndex->GetName() && i.m_sIdx1!=pJoinedIndex->GetName() )
		{
			sError.SetSprintf ( "JOIN ON table '%s' not found", i.m_sIdx1.cstr() );
			return false;
		}

		if ( i.m_sIdx2!=pIndex->GetName() && i.m_sIdx2!=pJoinedIndex->GetName() )
		{
			sError.SetSprintf ( "JOIN ON table '%s' not found", i.m_sIdx2.cstr() );
			return false;
		}
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////

ISphMatchSorter * CreateJoinSorter ( const CSphIndex * pIndex, const CSphIndex * pJoinedIndex, const SphQueueSettings_t & tSettings, const CSphQuery & tQuery, ISphMatchSorter * pSorter, bool bJoinedGroupSort, CSphString & sError )
{
	if ( !tSettings.m_pJoinArgs )
		return pSorter;

	if ( !CheckJoinOnFilters ( pIndex, pJoinedIndex, tQuery, sError ) )
	{
		SafeDelete(pSorter);
		return nullptr;
	}

	std::unique_ptr<JoinSorter_c> pJoinSorter = std::make_unique<JoinSorter_c> ( pIndex, pJoinedIndex, tQuery, pSorter, bJoinedGroupSort );
	if ( pJoinSorter->GetErrorFlag() )
	{
		sError = pJoinSorter->GetErrorMessage();
		pJoinSorter.reset();
	}

	return pJoinSorter.release();
}


bool CreateJoinMultiSorter ( const CSphIndex * pIndex, const CSphIndex * pJoinedIndex, const SphQueueSettings_t & tSettings, const CSphQuery & tQuery, VecTraits_T<ISphMatchSorter *> & dSorters, bool bJoinedGroupSort, CSphString & sError )
{
	if ( !tSettings.m_pJoinArgs )
		return true;

	if ( !CheckJoinOnFilters ( pIndex, pJoinedIndex, tQuery, sError ) )
		return false;

	// the idea is that 1st sorter does the join AND it also pushes joined matches to all other sorters
	// to avoid double push to 1..N sorters they are wrapped in a class that prevents pushing matches
	std::unique_ptr<JoinMultiSorter_c> pJoinSorter = std::make_unique<JoinMultiSorter_c> ( pIndex, pJoinedIndex, tQuery, dSorters, bJoinedGroupSort );
	if ( pJoinSorter->GetErrorFlag() )
	{
		sError = pJoinSorter->GetErrorMessage();
		pJoinSorter.reset();
	}

	dSorters[0] = pJoinSorter.release();

	for ( int i = 1; i < dSorters.GetLength(); i++ )
	{
		if ( !dSorters[i] )
			continue;

		dSorters[i] = new SorterWrapperNoPush_c ( dSorters[i] );
	}

	return true;
}
