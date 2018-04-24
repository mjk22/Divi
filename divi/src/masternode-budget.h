// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX Developers
// Copyright (c) 2017-2018 The Divi Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_BUDGET_H
#define MASTERNODE_BUDGET_H

#include "base58.h"
#include "init.h"
#include "key.h"
#include "main.h"
#include "masternode.h"
#include "net.h"
#include "sync.h"
#include "util.h"
#include <boost/lexical_cast.hpp>

using namespace std;

class CBudgetManager;
class CBudgetProposal;
class CBudgetProposalBroadcast;
class CTxBudgetPayment;

static const CAmount PROPOSAL_FEE_TX = (50 * COIN);
static const CAmount BUDGET_FEE_TX = (50 * COIN);
static const int64_t BUDGET_VOTE_UPDATE_MIN = 60 * 60;

CBudgetManager budget;

void DumpBudgets();

// Define amount of blocks in budget payment cycle
int GetBudgetPaymentCycleBlocks();

bool IsBudgetCollateralValid(uint256 nTxCollateralHash, uint256 nExpectedHash, std::string& strError, int64_t& nTime, int& nConf);

//
// CBudgetVote - Allow a masternode node to vote and broadcast throughout the network
//

class CBudgetVote
{
public:
    std::string address = "";
    uint256 nProposalHash;
    std::string nVote;
    int64_t nTime;
    std::vector<unsigned char> vchSig;
	bool fSynced = false;									//if we've sent this to our peers

	std::string ToString() { return address + nProposalHash.ToString() + boost::lexical_cast<std::string>(nVote) + boost::lexical_cast<std::string>(nTime); }
	void Relay();

    uint256 GetHash()
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << address << nProposalHash << nVote << nTime;
        return ss.GetHash();
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(address);
        READWRITE(nProposalHash);
        READWRITE(nVote);
        READWRITE(nTime);
        READWRITE(vchSig);
    }
};

/** Save Budget Manager (budget.dat)
 */
class CBudgetDB
{
private:
    boost::filesystem::path pathDB;
    std::string strMagicMessage;

public:
    enum ReadResult {
        Ok,
        FileError,
        HashReadError,
        IncorrectHash,
        IncorrectMagicMessage,
        IncorrectMagicNumber,
        IncorrectFormat
    };

    CBudgetDB();
    bool Write(const CBudgetManager& objToSave);
    ReadResult Read(CBudgetManager& objToLoad, bool fDryRun = false);
};


//
// Budget Manager : Contains all budget proposals and budget allocations
//
class CBudgetManager
{
private:
	mutable CCriticalSection cs_budget;				// critical section to protect the inner data structures

public:
    map<uint256, CBudgetProposal> mapProposals;
    std::map<uint256, CBudgetProposalBroadcast> mapSeenMasternodeBudgetProposals;
    std::map<uint256, CBudgetVote> mapSeenMasternodeBudgetVotes;

    void ResetSync();
    void MarkSynced();
    void Sync(CNode* node, uint256 nProp, bool fPartial = false);

    void ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);
    void NewBlock();
    CBudgetProposal* FindProposal(const std::string& strProposalName);
    CBudgetProposal* FindProposal(uint256 nHash);
	bool AddProposal(CBudgetProposal& budgetProposal);

    CAmount GetTotalBudget(int nHeight);
    std::vector<CBudgetProposal*> GetBudget();
    std::vector<CBudgetProposal*> GetAllProposals();
    bool IsBudgetPaymentBlock(int nBlockHeight);
    void SubmitFinalBudget();

    bool UpdateProposal(CBudgetVote& vote, CNode* pfrom, std::string& strError);
    bool PropExists(uint256 nHash);
    bool IsTransactionValid(const CTransaction& txNew, int nBlockHeight);
    void FillBlockPayee(CMutableTransaction& txNew, CAmount nFees, bool fProofOfStake);

    std::string ToString() const;


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(mapSeenMasternodeBudgetProposals);
        READWRITE(mapSeenMasternodeBudgetVotes);
        READWRITE(mapProposals);
    }
};


class CTxBudgetPayment
{
public:
    uint256 nProposalHash;
    CScript payee;
    CAmount nAmount;

    CTxBudgetPayment()
    {
        payee = CScript();
        nAmount = 0;
        nProposalHash = 0;
    }

    ADD_SERIALIZE_METHODS;

    //for saving to the serialized db
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(payee);
        READWRITE(nAmount);
        READWRITE(nProposalHash);
    }
};

//
// Budget Proposal : Contains the masternode votes for each budget
//

class CBudgetProposal
{
private:
    mutable CCriticalSection cs;			// critical section to protect the inner data structures

public:
    std::string strPropName;
    std::string strURL;
    int nBlockStart;
    int nBlockEnd;
    CAmount nAmount;
    CScript address;
    int64_t nTime;
    uint256 nFeeTXHash;
    map<uint256, CBudgetVote> mapVotes;

	CBudgetProposal::CBudgetProposal() {}

	CBudgetProposal::CBudgetProposal(std::string propNameIn, std::string URLIn, int blockStartIn, int blockEndIn, CAmount amountIn, CScript addrIn, uint256 feeTXHashIn)
	{
		strPropName = propNameIn; strURL = URLIn; nBlockStart = blockStartIn; nBlockEnd = blockEndIn; nAmount = amountIn; address = addrIn; nFeeTXHash = feeTXHashIn;
	}

	CBudgetProposal::CBudgetProposal(const CBudgetProposal& other)
	{
		strPropName = other.strPropName;
		strURL = other.strURL;
		nBlockStart = other.nBlockStart;
		nBlockEnd = other.nBlockEnd;
		address = other.address;
		nAmount = other.nAmount;
		nTime = other.nTime;
		nFeeTXHash = other.nFeeTXHash;
		mapVotes = other.mapVotes;
	}


    bool AddOrUpdateVote(CBudgetVote& vote, std::string& strError);
    bool IsValid(std::string& strError, bool fCheckCollateral = true);
	double GetVoteCount();

    void CleanAndRemove(bool fSignatureCheck);

    uint256 GetHash()
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << strPropName << strURL << nBlockStart << nBlockEnd << nAmount << address;
        return(ss.GetHash());
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        //for syncing with other clients & saving to the serialized db
        READWRITE(LIMITED_STRING(strProp, 20));
        READWRITE(LIMITED_STRING(strURL, 64));
        READWRITE(nTime);
        READWRITE(nBlockStart);
        READWRITE(nBlockEnd);
        READWRITE(nAmount);
        READWRITE(address);
        READWRITE(nTime);
        READWRITE(nFeeTXHash);
        //for saving to the serialized db only
        READWRITE(mapVotes);
    }
};

#endif
