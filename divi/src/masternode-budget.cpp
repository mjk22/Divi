// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX Developers
// Copyright (c) 2017-2018 The Divi Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "init.h"
#include "main.h"
#include "addrman.h"
#include "masternode-budget.h"
#include "masternode-sync.h"
#include "masternode.h"
#include "masternodeman.h"
#include "obfuscation.h"
#include "util.h"
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>

bool IsBudgetCollateralValid(uint256 nTxCollateralHash, uint256 nExpectedHash, std::string& strError, int64_t& nTime, int& nConf)
{
	CTransaction txCollateral;
	uint256 nBlockHash;
	if (!GetTransaction(nTxCollateralHash, txCollateral, nBlockHash, true)) {
		strError = strprintf("Can't find collateral tx %s", txCollateral.ToString());
		LogPrint("masternode", "CBudgetProposalBroadcast::IsBudgetCollateralValid - %s\n", strError);
		return false;
	}

	if (txCollateral.vout.size() < 1) return false;
	if (txCollateral.nLockTime != 0) return false;

	CScript findScript;
	findScript << OP_RETURN << ToByteVector(nExpectedHash);

	bool foundOpReturn = false;
	BOOST_FOREACH(const CTxOut o, txCollateral.vout) {
		if (!o.scriptPubKey.IsNormalPaymentScript() && !o.scriptPubKey.IsUnspendable()) {
			strError = strprintf("Invalid Script %s", txCollateral.ToString());
			LogPrint("masternode", "CBudgetProposalBroadcast::IsBudgetCollateralValid - %s\n", strError);
			return false;
		}
		if (o.scriptPubKey == findScript && o.nValue >= PROPOSAL_FEE_TX) foundOpReturn = true;
	}
	if (!foundOpReturn) {
		strError = strprintf("Couldn't find opReturn %s in %s", nExpectedHash.ToString(), txCollateral.ToString());
		LogPrint("masternode", "CBudgetProposalBroadcast::IsBudgetCollateralValid - %s\n", strError);
		return false;
	}

	// RETRIEVE CONFIRMATIONS AND NTIME
	/*
		- nTime starts as zero and is passed-by-reference out of this function and stored in the external proposal
		- nTime is never validated via the hashing mechanism and comes from a full-validated source (the blockchain)
	*/

	int conf = GetIXConfirmations(nTxCollateralHash);
	if (nBlockHash != uint256(0)) {
		BlockMap::iterator mi = mapBlockIndex.find(nBlockHash);
		if (mi != mapBlockIndex.end() && (*mi).second) {
			CBlockIndex* pindex = (*mi).second;
			if (chainActive.Contains(pindex)) {
				conf += chainActive.Height() - pindex->nHeight + 1;
				nTime = pindex->nTime;
			}
		}
	}

	nConf = conf;

	//if we're syncing we won't have swiftTX information, so accept 1 confirmation
	if (conf >= Params().Budget_Fee_Confirmations()) {
		return true;
	}
	else {
		strError = strprintf("Collateral requires at least %d confirmations - %d confirmations", Params().Budget_Fee_Confirmations(), conf);
		LogPrint("masternode", "CBudgetProposalBroadcast::IsBudgetCollateralValid - %s - %d confirmations\n", strError, conf);
		return false;
	}
}


//
// CBudgetDB
//

CBudgetDB::CBudgetDB()
{
	pathDB = GetDataDir() / "budget.dat";
	strMagicMessage = "MasternodeBudget";
}

bool CBudgetDB::Write(const CBudgetManager& objToSave)
{
	LOCK(objToSave.cs);

	int64_t nStart = GetTimeMillis();

	// serialize, checksum data up to that point, then append checksum
	CDataStream ssObj(SER_DISK, CLIENT_VERSION);
	ssObj << strMagicMessage;                   // masternode cache file specific magic message
	ssObj << FLATDATA(Params().MessageStart()); // network specific magic number
	ssObj << objToSave;
	uint256 hash = Hash(ssObj.begin(), ssObj.end());
	ssObj << hash;

	// open output file, and associate with CAutoFile
	FILE* file = fopen(pathDB.string().c_str(), "wb");
	CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
	if (fileout.IsNull())
		return error("%s : Failed to open file %s", __func__, pathDB.string());

	// Write and commit header, data
	try {
		fileout << ssObj;
	}
	catch (std::exception& e) {
		return error("%s : Serialize or I/O error - %s", __func__, e.what());
	}
	fileout.fclose();

	LogPrint("masternode", "Written info to budget.dat  %dms\n", GetTimeMillis() - nStart);

	return true;
}

CBudgetDB::ReadResult CBudgetDB::Read(CBudgetManager& objToLoad, bool fDryRun)
{
	LOCK(objToLoad.cs);

	int64_t nStart = GetTimeMillis();
	// open input file, and associate with CAutoFile
	FILE* file = fopen(pathDB.string().c_str(), "rb");
	CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
	if (filein.IsNull()) {
		error("%s : Failed to open file %s", __func__, pathDB.string());
		return FileError;
	}

	// use file size to size memory buffer
	int fileSize = boost::filesystem::file_size(pathDB);
	int dataSize = fileSize - sizeof(uint256);
	// Don't try to resize to a negative number if file is small
	if (dataSize < 0)
		dataSize = 0;
	vector<unsigned char> vchData;
	vchData.resize(dataSize);
	uint256 hashIn;

	// read data and checksum from file
	try {
		filein.read((char*)&vchData[0], dataSize);
		filein >> hashIn;
	}
	catch (std::exception& e) {
		error("%s : Deserialize or I/O error - %s", __func__, e.what());
		return HashReadError;
	}
	filein.fclose();

	CDataStream ssObj(vchData, SER_DISK, CLIENT_VERSION);

	// verify stored checksum matches input data
	uint256 hashTmp = Hash(ssObj.begin(), ssObj.end());
	if (hashIn != hashTmp) {
		error("%s : Checksum mismatch, data corrupted", __func__);
		return IncorrectHash;
	}


	unsigned char pchMsgTmp[4];
	std::string strMagicMessageTmp;
	try {
		// de-serialize file header (masternode cache file specific magic message) and ..
		ssObj >> strMagicMessageTmp;

		// ... verify the message matches predefined one
		if (strMagicMessage != strMagicMessageTmp) {
			error("%s : Invalid masternode cache magic message", __func__);
			return IncorrectMagicMessage;
		}


		// de-serialize file header (network specific magic number) and ..
		ssObj >> FLATDATA(pchMsgTmp);

		// ... verify the network matches ours
		if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp))) {
			error("%s : Invalid network magic number", __func__);
			return IncorrectMagicNumber;
		}

		// de-serialize data into CBudgetManager object
		ssObj >> objToLoad;
	}
	catch (std::exception& e) {
		objToLoad.Clear();
		error("%s : Deserialize or I/O error - %s", __func__, e.what());
		return IncorrectFormat;
	}

	LogPrint("masternode", "Loaded info from budget.dat  %dms\n", GetTimeMillis() - nStart);
	LogPrint("masternode", "  %s\n", objToLoad.ToString());
	return Ok;
}

void DumpBudgets()
{
	int64_t nStart = GetTimeMillis();

	CBudgetDB budgetdb;
	CBudgetManager tempBudget;

	LogPrint("masternode", "Verifying budget.dat format...\n");
	CBudgetDB::ReadResult readResult = budgetdb.Read(tempBudget, true);
	// there was an error and it was not an error on file opening => do not proceed
	if (readResult == CBudgetDB::FileError)
		LogPrint("masternode", "Missing budgets file - budget.dat, will try to recreate\n");
	else if (readResult != CBudgetDB::Ok) {
		LogPrint("masternode", "Error reading budget.dat: ");
		if (readResult == CBudgetDB::IncorrectFormat)
			LogPrint("masternode", "magic is ok but data has invalid format, will try to recreate\n");
		else {
			LogPrint("masternode", "file format is unknown or invalid, please fix it manually\n");
			return;
		}
	}
	LogPrint("masternode", "Writting info to budget.dat...\n");
	budgetdb.Write(budget);

	LogPrint("masternode", "Budget dump finished  %dms\n", GetTimeMillis() - nStart);
}

bool CBudgetManager::AddProposal(CBudgetProposal& budgetProposal)
{
	LOCK(cs);
	std::string strError = "";
	if (!budgetProposal.IsValid(strError)) {
		LogPrint("masternode", "CBudgetManager::AddProposal - invalid budget proposal - %s\n", strError);
		return false;
	}

	if (mapProposals.count(budgetProposal.GetHash())) {
		return false;
	}

	mapProposals.insert(make_pair(budgetProposal.GetHash(), budgetProposal));
	LogPrint("masternode", "CBudgetManager::AddProposal - proposal %s added\n", budgetProposal.GetName().c_str());
	return true;
}


CBudgetProposal* CBudgetManager::FindProposal(const std::string& strProposalName)
{
	//find the prop with the highest yes count

	int nYesCount = -99999;
	CBudgetProposal* pbudgetProposal = NULL;

	std::map<uint256, CBudgetProposal>::iterator it = mapProposals.begin();
	while (it != mapProposals.end()) {
		if ((*it).second.strProposalName == strProposalName && (*it).second.GetYeas() > nYesCount) {
			pbudgetProposal = &((*it).second);
			nYesCount = pbudgetProposal->GetYeas();
		}
		++it;
	}

	if (nYesCount == -99999) return NULL;

	return pbudgetProposal;
}

CBudgetProposal* CBudgetManager::FindProposal(uint256 nHash)
{
	LOCK(cs);

	if (mapProposals.count(nHash))
		return &mapProposals[nHash];

	return NULL;
}

bool CBudgetManager::IsBudgetPaymentBlock(int nBlockHeight)
{
	int nHighestCount = -1;
	int nFivePercent = mnodeman.CountEnabled(ActiveProtocol()) / 20;

	std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
	while (it != mapFinalizedBudgets.end()) {
		CFinalizedBudget* pfinalizedBudget = &((*it).second);
		if (pfinalizedBudget->GetVoteCount() > nHighestCount &&
			nBlockHeight >= pfinalizedBudget->GetBlockStart() &&
			nBlockHeight <= pfinalizedBudget->GetBlockEnd()) {
			nHighestCount = pfinalizedBudget->GetVoteCount();
		}

		++it;
	}

	LogPrint("masternode", "CBudgetManager::IsBudgetPaymentBlock() - nHighestCount: %lli, 5%% of Masternodes: %lli. Number of budgets: %lli\n",
		nHighestCount, nFivePercent, mapFinalizedBudgets.size());

	// If budget doesn't have 5% of the network votes, then we should pay a masternode instead
	if (nHighestCount > nFivePercent) return true;

	return false;
}

std::vector<CBudgetProposal*> CBudgetManager::GetAllProposals()
{
	LOCK(cs);

	std::vector<CBudgetProposal*> vBudgetProposalRet;

	std::map<uint256, CBudgetProposal>::iterator it = mapProposals.begin();
	while (it != mapProposals.end()) {
		(*it).second.CleanAndRemove(false);

		CBudgetProposal* pbudgetProposal = &((*it).second);
		vBudgetProposalRet.push_back(pbudgetProposal);

		++it;
	}

	return vBudgetProposalRet;
}

//
// Sort by votes, if there's a tie sort by their feeHash TX
//
struct sortProposalsByVotes {
	bool operator()(const std::pair<CBudgetProposal*, int>& left, const std::pair<CBudgetProposal*, int>& right)
	{
		if (left.second != right.second)
			return (left.second > right.second);
		return (left.first->nFeeTXHash > right.first->nFeeTXHash);
	}
};

//Need to review this function
std::vector<CBudgetProposal*> CBudgetManager::GetBudget()
{
	LOCK(cs);

	// ------- Sort budgets by Yes Count

	std::vector<std::pair<CBudgetProposal*, int> > vBudgetPorposalsSort;

	std::map<uint256, CBudgetProposal>::iterator it = mapProposals.begin();
	while (it != mapProposals.end()) {
		(*it).second.CleanAndRemove(false);
		vBudgetPorposalsSort.push_back(make_pair(&((*it).second), (*it).second.GetYeas() - (*it).second.GetNays()));
		++it;
	}

	std::sort(vBudgetPorposalsSort.begin(), vBudgetPorposalsSort.end(), sortProposalsByVotes());

	// ------- Grab The Budgets In Order

	std::vector<CBudgetProposal*> vBudgetProposalsRet;

	CAmount nBudgetAllocated = 0;
	CBlockIndex* pindexPrev = chainActive.Tip();
	if (pindexPrev == NULL) return vBudgetProposalsRet;

	int nBlockStart = pindexPrev->nHeight - pindexPrev->nHeight % GetBudgetPaymentCycleBlocks() + GetBudgetPaymentCycleBlocks();
	int nBlockEnd = nBlockStart + GetBudgetPaymentCycleBlocks() - 1;
	CAmount nTotalBudget = GetTotalBudget(nBlockStart);


	std::vector<std::pair<CBudgetProposal*, int> >::iterator it2 = vBudgetPorposalsSort.begin();
	while (it2 != vBudgetPorposalsSort.end()) {
		CBudgetProposal* pbudgetProposal = (*it2).first;

		LogPrint("masternode", "CBudgetManager::GetBudget() - Processing Budget %s\n", pbudgetProposal->strProposalName.c_str());
		//prop start/end should be inside this period
		if (pbudgetProposal->nBlockStart <= nBlockStart &&
			pbudgetProposal->nBlockEnd >= nBlockEnd &&
			pbudgetProposal->GetYeas() - pbudgetProposal->GetNays() > mnodeman.CountEnabled(ActiveProtocol()) / 10 &&
			pbudgetProposal->IsEstablished()) {

			LogPrint("masternode", "CBudgetManager::GetBudget() -   Check 1 passed: %ld <= %ld | %ld >= %ld | Yeas=%d Nays=%d Count=%d | established=%d\n",
				pbudgetProposal->nBlockStart, nBlockStart, pbudgetProposal->nBlockEnd,
				nBlockEnd, pbudgetProposal->GetYeas(), pbudgetProposal->GetNays(), mnodeman.CountEnabled(ActiveProtocol()) / 10,
				pbudgetProposal->IsEstablished());

			if (pbudgetProposal->GetAmount() + nBudgetAllocated <= nTotalBudget) {
				pbudgetProposal->SetAllotted(pbudgetProposal->GetAmount());
				nBudgetAllocated += pbudgetProposal->GetAmount();
				vBudgetProposalsRet.push_back(pbudgetProposal);
				LogPrint("masternode", "CBudgetManager::GetBudget() -     Check 2 passed: Budget added\n");
			}
			else {
				pbudgetProposal->SetAllotted(0);
				LogPrint("masternode", "CBudgetManager::GetBudget() -     Check 2 failed: no amount allotted\n");
			}
		}
		else {
			LogPrint("masternode", "CBudgetManager::GetBudget() -   Check 1 failed: %ld <= %ld | %ld >= %ld | Yeas=%d Nays=%d Count=%d | established=%d\n",
				pbudgetProposal->nBlockStart, nBlockStart, pbudgetProposal->nBlockEnd,
				nBlockEnd, pbudgetProposal->GetYeas(), pbudgetProposal->GetNays(), mnodeman.CountEnabled(ActiveProtocol()) / 10,
				pbudgetProposal->IsEstablished());
		}

		++it2;
	}

	return vBudgetProposalsRet;
}

struct sortFinalizedBudgetsByVotes {
	bool operator()(const std::pair<CFinalizedBudget*, int>& left, const std::pair<CFinalizedBudget*, int>& right)
	{
		return left.second > right.second;
	}
};

void CBudgetManager::NewBlock()
{
	//this function should be called 1/14 blocks, allowing up to 100 votes per day on all proposals
	if (chainActive.Height() % 14 != 0) return;

	TRY_LOCK(cs, fBudgetNewBlock);
	if (!fBudgetNewBlock) return;

	if (masternodeSync.RequestedMasternodeAssets <= MASTERNODE_SYNC_BUDGET) return;

	// incremental sync with our peers
	if (masternodeSync.IsSynced()) {
		LogPrint("masternode", "CBudgetManager::NewBlock - incremental sync started\n");
		if (chainActive.Height() % 1440 == rand() % 1440) {
			mapSeenMasternodeBudgetProposals.clear();
			mapSeenMasternodeBudgetVotes.clear();
			ResetSync();
		}

		LOCK(cs_vNodes);
		BOOST_FOREACH(CNode* pnode, vNodes)
			if (pnode->nVersion >= ActiveProtocol())
				Sync(pnode, 0, true);

		MarkSynced();
	}

	LogPrint("masternode", "CBudgetManager::NewBlock - PASSED\n");
}

void CBudgetManager::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
	// lite mode is not supported
	if (fLiteMode) return;
	if (!masternodeSync.IsBlockchainSynced()) return;

	LOCK(cs_budget);

	if (strCommand == "mnvs") { //Masternode vote sync
		uint256 nProp;
		vRecv >> nProp;

		if (Params().NetworkID() == CBaseChainParams::MAIN) {
			if (nProp == 0) {
				if (pfrom->HasFulfilledRequest("mnvs")) {
					LogPrint("masternode", "mnvs - peer already asked me for the list\n");
					Misbehaving(pfrom->GetId(), 20);
					return;
				}
				pfrom->FulfilledRequest("mnvs");
			}
		}

		Sync(pfrom, nProp);
		LogPrint("mnbudget", "mnvs - Sent Masternode votes to peer %i\n", pfrom->GetId());
	}

	if (strCommand == "mprop") { //Masternode Proposal
		CBudgetProposal budgetProposal;
		vRecv >> budgetProposal;

		if (mapSeenMasternodeBudgetProposals.count(budgetProposal.GetHash())) {
			masternodeSync.AddedBudgetItem(budgetProposal.GetHash());
			return;
		}

		std::string strError = "";
		int nConf = 0;
		if (!IsBudgetCollateralValid(budgetProposal.nFeeTXHash, budgetProposal.GetHash(), strError, budgetProposal.nTime, nConf)) {
			LogPrint("masternode", "Proposal FeeTX is not valid - %s - %s\n", budgetProposal.nFeeTXHash.ToString(), strError);
			return;
		}

		mapSeenMasternodeBudgetProposals.insert(make_pair(budgetProposal.GetHash(), budgetProposal));

		if (!budgetProposal.IsValid(strError)) {
			LogPrint("masternode", "mprop - invalid budget proposal - %s\n", strError);
			return;
		}

		CBudgetProposal budgetProposal(budgetProposal);
		if (AddProposal(budgetProposal)) {
			budgetProposal.Relay();
		}
		masternodeSync.AddedBudgetItem(budgetProposal.GetHash());

		LogPrint("masternode", "mprop - new budget - %s\n", budgetProposal.GetHash().ToString());
	}

	if (strCommand == "mvote") { //Masternode Vote
		CBudgetVote vote;
		vRecv >> vote;

		if (mapSeenMasternodeBudgetVotes.count(vote.GetHash())) {
			masternodeSync.AddedBudgetItem(vote.GetHash());
			return;
		}

		// ToDo: Reimplement for addresses rather than nodes
		//CMasternode* pmn = mnodeman.Find(vote.vin);
		//if (pmn == NULL) {
		//    LogPrint("masternode","mvote - unknown masternode - vin: %s\n", vote.vin.prevout.hash.ToString());
		//    mnodeman.AskForMN(pfrom, vote.vin);
		//    return;
		//}

		mapSeenMasternodeBudgetVotes.insert(make_pair(vote.GetHash(), vote));
		if (!obfuScationSigner.VerifyMessage(vote.address, vote.ToString(), vote.vchSig)) return;

		std::string strError = "";
		if (UpdateProposal(vote, pfrom, strError)) {
			vote.Relay();
			masternodeSync.AddedBudgetItem(vote.GetHash());
			LogPrint("masternode", "mvote - new budget vote for budget %s - %s\n", vote.nProposalHash.ToString(), vote.GetHash().ToString());
		}
		else LogPrint("masternode", "Error updating vote for budget %s - %s\n", vote.nProposalHash.ToString(), pfrom->addrName);

	}
}

bool CBudgetManager::PropExistsX(uint256 nHash)
{
	if (mapProposals.count(nHash)) return true;
	return false;
}

//mark that a full sync is needed
void CBudgetManager::ResetSync()
{
	LOCK(cs);
	std::map<uint256, CBudgetProposalBroadcast>::iterator it1 = mapSeenMasternodeBudgetProposals.begin();
	while (it1 != mapSeenMasternodeBudgetProposals.end()) {
		CBudgetProposal* pbudgetProposal = FindProposal((*it1).first);
		if (pbudgetProposal) {
			//mark votes
			std::map<uint256, CBudgetVote>::iterator it2 = pbudgetProposal->mapVotes.begin();
			while (it2 != pbudgetProposal->mapVotes.end()) {
				(*it2).second.fSynced = false;
				++it2;
			}
		}
		++it1;
	}
}

void CBudgetManager::MarkSynced()  // Mark that we've sent all valid items
{
	LOCK(cs);
	std::map<uint256, CBudgetProposalBroadcast>::iterator it1 = mapSeenMasternodeBudgetProposals.begin();
	while (it1 != mapSeenMasternodeBudgetProposals.end()) {
		CBudgetProposal* pbudgetProposal = FindProposal((*it1).first);
		if (pbudgetProposal) {
			std::map<uint256, CBudgetVote>::iterator it2 = pbudgetProposal->mapVotes.begin();
			while (it2 != pbudgetProposal->mapVotes.end()) { (*it2).second.fSynced = true; ++it2; }
		}
		++it1;
	}
}


void CBudgetManager::Sync(CNode* pfrom, uint256 nProp, bool fPartial)
{
	LOCK(cs);

	/*
		Sync with a client on the network
		--
		This code checks each of the hash maps for all known budget proposals, then checks them against the
		budget object to see if they're OK. If all checks pass, we'll send it to the peer.
	*/

	int nInvCount = 0;

	std::map<uint256, CBudgetProposalBroadcast>::iterator it1 = mapSeenMasternodeBudgetProposals.begin();
	while (it1 != mapSeenMasternodeBudgetProposals.end()) {
		CBudgetProposal* pbudgetProposal = FindProposal((*it1).first);
		if (pbudgetProposal && (nProp == 0 || (*it1).first == nProp)) {
			pfrom->PushInventory(CInv(MSG_BUDGET_PROPOSAL, (*it1).second.GetHash()));
			nInvCount++;

			//send votes
			std::map<uint256, CBudgetVote>::iterator it2 = pbudgetProposal->mapVotes.begin();
			while (it2 != pbudgetProposal->mapVotes.end()) {
				if ((fPartial && !(*it2).second.fSynced) || !fPartial) {
					pfrom->PushInventory(CInv(MSG_BUDGET_VOTE, (*it2).second.GetHash()));
					nInvCount++;
				}
				++it2;
			}
		}
		++it1;
	}
	pfrom->PushMessage("ssc", MASTERNODE_SYNC_BUDGET_PROP, nInvCount);
	LogPrint("mnbudget", "CBudgetManager::Sync - sent %d items\n", nInvCount);
}

bool CBudgetManager::UpdateProposal(CBudgetVote& vote, CNode* pfrom, std::string& strError)
{
	LOCK(cs);

	if (!mapProposals.count(vote.nProposalHash)) {
		if (pfrom) {
			// only ask for missing items after our syncing process is complete --
			//   otherwise we'll think a full sync succeeded when they return a result
			if (!masternodeSync.IsSynced()) return false;

			LogPrint("masternode", "CBudgetManager::UpdateProposal - Unknown proposal %d, asking for source proposal\n", vote.nProposalHash.ToString());
			mapOrphanMasternodeBudgetVotes[vote.nProposalHash] = vote;

			if (!askedForSourceProposalOrBudget.count(vote.nProposalHash)) {
				pfrom->PushMessage("mnvs", vote.nProposalHash);
				askedForSourceProposalOrBudget[vote.nProposalHash] = GetTime();
			}
		}
		strError = "Proposal not found!";
		return false;
	}
	return mapProposals[vote.nProposalHash].AddOrUpdateVote(vote, strError);
}

bool CBudgetProposal::IsValid(std::string& strError, bool fCheckCollateral)
{
	if (GetNays() - GetYeas() > mnodeman.CountEnabled(ActiveProtocol()) / 10) {
		strError = "Proposal " + strProposalName + ": Active removal";
		return false;
	}

	if (nBlockStart < 0) {
		strError = "Invalid Proposal";
		return false;
	}

	if (nBlockEnd < nBlockStart) {
		strError = "Proposal " + strProposalName + ": Invalid nBlockEnd (end before start)";
		return false;
	}

	if (nAmount < 10 * COIN) {
		strError = "Proposal " + strProposalName + ": Invalid nAmount";
		return false;
	}

	if (address == CScript()) {
		strError = "Proposal " + strProposalName + ": Invalid Payment Address";
		return false;
	}

	if (fCheckCollateral) {
		int nConf = 0;
		if (!IsBudgetCollateralValid(nFeeTXHash, GetHash(), strError, nTime, nConf)) {
			strError = "Proposal " + strProposalName + ": Invalid collateral";
			return false;
		}
	}

	/*
		TODO: There might be an issue with multisig in the coinbase on mainnet, we will add support for it in a future release.
	*/
	if (address.IsPayToScriptHash()) {
		strError = "Proposal " + strProposalName + ": Multisig is not currently supported.";
		return false;
	}

	//if proposal doesn't gain traction within 2 weeks, remove it
	// nTime not being saved correctly
	// -- TODO: We should keep track of the last time the proposal was valid, if it's invalid for 2 weeks, erase it
	// if(nTime + (60*60*24*2) < GetAdjustedTime()) {
	//     if(GetYeas()-GetNays() < (mnodeman.CountEnabled(ActiveProtocol())/10)) {
	//         strError = "Not enough support";
	//         return false;
	//     }
	// }

	//can only pay out 10% of the possible coins (min value of coins)
	if (nAmount > budget.GetTotalBudget(nBlockStart)) {
		strError = "Proposal " + strProposalName + ": Payment more than max";
		return false;
	}

	CBlockIndex* pindexPrev = chainActive.Tip();
	if (pindexPrev == NULL) {
		strError = "Proposal " + strProposalName + ": Tip is NULL";
		return true;
	}

	// Calculate maximum block this proposal will be valid, which is start of proposal + (number of payments * cycle)
	int nProposalEnd = GetBlockStart() + (GetBudgetPaymentCycleBlocks() * GetTotalPaymentCount());

	// if (GetBlockEnd() < pindexPrev->nHeight - GetBudgetPaymentCycleBlocks() / 2) {
	if (nProposalEnd < pindexPrev->nHeight) {
		strError = "Proposal " + strProposalName + ": Invalid nBlockEnd (" + std::to_string(nProposalEnd) + ") < current height (" + std::to_string(pindexPrev->nHeight) + ")";
		return false;
	}

	return true;
}

bool CBudgetProposal::AddOrUpdateVote(CBudgetVote& vote, std::string& strError)
{
	std::string strAction = "New vote inserted:";
	LOCK(cs);

	if (mapVotes.count(vote.address)) {
		if (mapVotes[vote.address].nTime > vote.nTime) {
			strError = strprintf("new vote older than existing vote - %s\n", vote.GetHash().ToString());
			LogPrint("mnbudget", "CBudgetProposal::AddOrUpdateVote - %s\n", strError);
			return false;
		}
		if (vote.nTime - mapVotes[vote.address].nTime < BUDGET_VOTE_UPDATE_MIN) {
			strError = strprintf("time between votes is too soon - %s - %lli sec < %lli sec\n", vote.GetHash().ToString(), vote.nTime - mapVotes[vote.address].nTime, BUDGET_VOTE_UPDATE_MIN);
			LogPrint("mnbudget", "CBudgetProposal::AddOrUpdateVote - %s\n", strError);
			return false;
		}
		strAction = "Existing vote updated:";
	}

	if (vote.nTime > GetTime() + (60 * 60)) {
		strError = strprintf("new vote is too far ahead of current time - %s - nTime %lli - Max Time %lli\n", vote.GetHash().ToString(), vote.nTime, GetTime() + (60 * 60));
		LogPrint("mnbudget", "CBudgetProposal::AddOrUpdateVote - %s\n", strError);
		return false;
	}

	mapVotes[vote.address] = vote;
	LogPrint("mnbudget", "CBudgetProposal::AddOrUpdateVote - %s %s\n", strAction.c_str(), vote.GetHash().ToString().c_str());

	return true;
}

double CBudgetProposal::GetVoteCount()
{
	int yeas = 0;
	int nays = 0;
	int abstains = 0;

	std::map<uint256, CBudgetVote>::iterator it = mapVotes.begin();
	while (it != mapVotes.end()) {
		if ((*it).second.nVote == "yes") yeas++;
		if ((*it).second.nVote == "no") nays++;
		if ((*it).second.nVote == 0) abstains++;
		++it;
	}
	if (yeas + nays == 0) return 0.0f;

	return ((double)(yeas) / (double)(yeas + nays));
}

void CBudgetProposal::Relay()
{
	CInv inv(MSG_BUDGET_PROPOSAL, GetHash());
	RelayInv(inv);
}

void CBudgetVote::Relay()
{
	CInv inv(MSG_BUDGET_VOTE, GetHash());
	RelayInv(inv);
}


std::string CBudgetManager::ToString() const
{
	std::ostringstream info;

	info << "Proposals: " << (int)mapProposals.size() << ", Seen Budgets: " << (int)mapSeenMasternodeBudgetProposals.size() << ", Seen Budget Votes: " << (int)mapSeenMasternodeBudgetVotes.size();

	return info.str();
}
