// Copyright (c) 2014-2015 The Dash Developers
// Copyright (c) 2015-2017 The PIVX Developers
// Copyright (c) 2017-2018 The Divi Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// ToDo: Refactor everyhing to use masternode-budget.cpp routines instead of re-inventing the wheel
// ToDo: Change superblocks to be independent of blocks

#include "activemasternode.h"
#include "db.h"
#include "init.h"
#include "main.h"
#include "masternode-budget.h"
#include "masternode-payments.h"
#include "masternodeconfig.h"
#include "masternodeman.h"
#include "rpcserver.h"
#include "utilmoneystr.h"

#include <fstream>
using namespace json_spirit;
using namespace std;

void proposalToJSON(CBudgetProposal* pbudgetProposal, Object& bObj)
{
	CTxDestination address1;
	ExtractDestination(pbudgetProposal->GetPayee(), address1);
	CBitcoinAddress address2(address1);

	bObj.push_back(Pair("Name", pbudgetProposal->GetName()));
	bObj.push_back(Pair("URL", pbudgetProposal->GetURL()));
	bObj.push_back(Pair("Hash", pbudgetProposal->GetHash().ToString()));
	bObj.push_back(Pair("FeeHash", pbudgetProposal->nFeeTXHash.ToString()));
	bObj.push_back(Pair("BlockStart", (int64_t)pbudgetProposal->GetBlockStart()));
	bObj.push_back(Pair("BlockEnd", (int64_t)pbudgetProposal->GetBlockEnd()));
	bObj.push_back(Pair("TotalPaymentCount", (int64_t)pbudgetProposal->GetTotalPaymentCount()));
	bObj.push_back(Pair("RemainingPaymentCount", (int64_t)pbudgetProposal->GetRemainingPaymentCount()));
	bObj.push_back(Pair("PaymentAddress", address2.ToString()));
	bObj.push_back(Pair("Ratio", pbudgetProposal->GetRatio()));
	bObj.push_back(Pair("Yeas", (int64_t)pbudgetProposal->GetYeas()));
	bObj.push_back(Pair("Nays", (int64_t)pbudgetProposal->GetNays()));
	bObj.push_back(Pair("Abstains", (int64_t)pbudgetProposal->GetAbstains()));
	bObj.push_back(Pair("TotalPayment", ValueFromAmount(pbudgetProposal->GetAmount() * pbudgetProposal->GetTotalPaymentCount())));
	bObj.push_back(Pair("MonthlyPayment", ValueFromAmount(pbudgetProposal->GetAmount())));
	bObj.push_back(Pair("IsEstablished", pbudgetProposal->IsEstablished()));

	std::string strError = "";
	bObj.push_back(Pair("IsValid", pbudgetProposal->IsValid(strError)));
	bObj.push_back(Pair("IsValidReason", strError.c_str()));
}

Value prepareproposal(const Array& params, bool fHelp)
{
	int nBlockMin = 0;
	CBlockIndex* pindexPrev = chainActive.Tip();

	if (fHelp || params.size() != 6)
		throw runtime_error(
			"prepareproposal \"proposal-name\" \"url\" payment-count block-start \"divi-address\" monthy-payment\n"
			"\nPrepare proposal for network by signing and creating tx\n"

			"\nArguments:\n"
			"1. \"proposal-name\":  (string, required) Desired proposal name (20 character limit)\n"
			"2. \"url\":            (string, required) URL of proposal details (64 character limit)\n"
			"3. payment-count:    (numeric, required) Total number of monthly payments\n"
			"4. block-start:      (numeric, required) Starting super block height\n"
			"5. \"divi-address\":   (string, required) DIVI address to send payments to\n"
			"6. monthly-payment:  (numeric, required) Monthly payment amount\n"

			"\nResult:\n"
			"\"xxxx\"       (string) proposal fee hash (if successful) or error message (if failed)\n"
			"\nExamples:\n" +
			HelpExampleCli("preparebudget", "\"test-proposal\" \"https://forum.diviproject.org/t/test-proposal\" 2 820800 \"D9oc6C3dttUbv8zd7zGNq1qKBGf4ZQ1XEE\" 500") +
			HelpExampleRpc("preparebudget", "\"test-proposal\" \"https://forum.diviproject.org/t/test-proposal\" 2 820800 \"D9oc6C3dttUbv8zd7zGNq1qKBGf4ZQ1XEE\" 500"));

	if (pwalletMain->IsLocked())
		throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

	std::string strProposalName = SanitizeString(params[0].get_str());
	if (strProposalName.size() > 20)
		throw runtime_error("Invalid proposal name, limit of 20 characters.");

	std::string strURL = SanitizeString(params[1].get_str());
	if (strURL.size() > 64)
		throw runtime_error("Invalid url, limit of 64 characters.");

	int nPaymentCount = params[2].get_int();
	if (nPaymentCount < 1)
		throw runtime_error("Invalid payment count, must be more than zero.");

	// Start must be in the next budget cycle
	if (pindexPrev != NULL) nBlockMin = pindexPrev->nHeight - pindexPrev->nHeight % GetBudgetPaymentCycleBlocks() + GetBudgetPaymentCycleBlocks();

	int nBlockStart = params[3].get_int();
	if (nBlockStart % GetBudgetPaymentCycleBlocks() != 0) {
		int nNext = pindexPrev->nHeight - pindexPrev->nHeight % GetBudgetPaymentCycleBlocks() + GetBudgetPaymentCycleBlocks();
		throw runtime_error(strprintf("Invalid block start - must be a budget cycle block. Next valid block: %d", nNext));
	}

	int nBlockEnd = nBlockStart + GetBudgetPaymentCycleBlocks() * nPaymentCount; // End must be AFTER current cycle

	if (nBlockStart < nBlockMin)
		throw runtime_error("Invalid block start, must be more than current height.");

	if (nBlockEnd < pindexPrev->nHeight)
		throw runtime_error("Invalid ending block, starting block + (payment_cycle*payments) must be more than current height.");

	CBitcoinAddress address(params[4].get_str());
	if (!address.IsValid())
		throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Divi address");

	// Parse Divi address
	CScript scriptPubKey = GetScriptForDestination(address.Get());
	CAmount nAmount = AmountFromValue(params[5]);

	//*************************************************************************

	// create transaction 15 minutes into the future, to allow for confirmation time
	CBudgetProposal budgetProposal(strProposalName, strURL, nPaymentCount, scriptPubKey, nAmount, nBlockStart, 0);

	std::string strError = "";
	if (!budgetProposal.IsValid(strError, false))
		throw runtime_error("Proposal is not valid - " + budgetProposal.GetHash().ToString() + " - " + strError);

	bool useIX = false; //true;
	// if (params.size() > 7) {
	//     if(params[7].get_str() != "false" && params[7].get_str() != "true")
	//         return "Invalid use_ix, must be true or false";
	//     useIX = params[7].get_str() == "true" ? true : false;
	// }

	CWalletTx wtx;
	if (!pwalletMain->GetBudgetSystemCollateralTX(wtx, budgetProposal.GetHash(), useIX)) {
		throw runtime_error("Error making collateral transaction for proposal. Please check your wallet balance.");
	}

	// make our change address
	CReserveKey reservekey(pwalletMain);
	//send the tx to the network
	pwalletMain->CommitTransaction(wtx, reservekey, useIX ? "ix" : "tx");

	return wtx.GetHash().ToString();
}

Value submitproposal(const Array& params, bool fHelp)
{
	int nBlockMin = 0;
	CBlockIndex* pindexPrev = chainActive.Tip();

	if (fHelp || params.size() != 7)
		throw runtime_error(
			"submitproposal \"proposal-name\" \"url\" payment-count block-start \"divi-address\" monthy-payment \"fee-tx\"\n"
			"\nSubmit proposal to the network\n"

			"\nArguments:\n"
			"1. \"proposal-name\":  (string, required) Desired proposal name (20 character limit)\n"
			"2. \"url\":            (string, required) URL of proposal details (64 character limit)\n"
			"3. payment-count:    (numeric, required) Total number of monthly payments\n"
			"4. block-start:      (numeric, required) Starting super block height\n"
			"5. \"divi-address\":   (string, required) DIVI address to send payments to\n"
			"6. monthly-payment:  (numeric, required) Monthly payment amount\n"
			"7. \"fee-tx\":         (string, required) Transaction hash from preparebudget command\n"

			"\nResult:\n"
			"\"xxxx\"       (string) proposal hash (if successful) or error message (if failed)\n"
			"\nExamples:\n" +
			HelpExampleCli("submitbudget", "\"test-proposal\" \"https://forum.diviproject.org/t/test-proposal\" 2 820800 \"D9oc6C3dttUbv8zd7zGNq1qKBGf4ZQ1XEE\" 500") +
			HelpExampleRpc("submitbudget", "\"test-proposal\" \"https://forum.diviproject.org/t/test-proposal\" 2 820800 \"D9oc6C3dttUbv8zd7zGNq1qKBGf4ZQ1XEE\" 500"));

	// Check these inputs the same way we check the vote commands:
	// **********************************************************

	std::string strProposalName = SanitizeString(params[0].get_str());
	if (strProposalName.size() > 20)
		throw runtime_error("Invalid proposal name, limit of 20 characters.");

	std::string strURL = SanitizeString(params[1].get_str());
	if (strURL.size() > 64)
		throw runtime_error("Invalid url, limit of 64 characters.");

	int nPaymentCount = params[2].get_int();
	if (nPaymentCount < 1)
		throw runtime_error("Invalid payment count, must be more than zero.");

	// Start must be in the next budget cycle
	if (pindexPrev != NULL) nBlockMin = pindexPrev->nHeight - pindexPrev->nHeight % GetBudgetPaymentCycleBlocks() + GetBudgetPaymentCycleBlocks();

	int nBlockStart = params[3].get_int();
	if (nBlockStart % GetBudgetPaymentCycleBlocks() != 0) {
		int nNext = pindexPrev->nHeight - pindexPrev->nHeight % GetBudgetPaymentCycleBlocks() + GetBudgetPaymentCycleBlocks();
		throw runtime_error(strprintf("Invalid block start - must be a budget cycle block. Next valid block: %d", nNext));
	}

	int nBlockEnd = nBlockStart + (GetBudgetPaymentCycleBlocks() * nPaymentCount); // End must be AFTER current cycle

	if (nBlockStart < nBlockMin)
		throw runtime_error("Invalid block start, must be more than current height.");

	if (nBlockEnd < pindexPrev->nHeight)
		throw runtime_error("Invalid ending block, starting block + (payment_cycle*payments) must be more than current height.");

	CBitcoinAddress address(params[4].get_str());
	if (!address.IsValid())
		throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Divi address");

	CScript scriptPubKey = GetScriptForDestination(address.Get());
	CAmount nAmount = AmountFromValue(params[5]);
	uint256 hash = ParseHashV(params[6], "parameter 1");

	//create the proposal incase we're the first to make it
	CBudgetProposal budgetProposal(strProposalName, strURL, nBlockStart, nBlockStart + nPaymentCount - 1, nAmount, scriptPubKey, hash);

	std::string strError = "";
	int nConf = 0;
	if (!IsBudgetCollateralValid(hash, budgetProposal.GetHash(), strError, budgetProposal.nTime, nConf)) {
		throw runtime_error("Proposal FeeTX is not valid - " + hash.ToString() + " - " + strError);
	}

	if (!masternodeSync.IsBlockchainSynced()) {
		throw runtime_error("Must wait for client to sync with masternode network. Try again in a minute or so.");
	}

	if (!budgetProposal.IsValid(strError)) {
		return "Proposal is not valid - " + budgetProposal.GetHash().ToString() + " - " + strError;
	}

	budget.mapSeenMasternodeBudgetProposals.insert(make_pair(budgetProposal.GetHash(), budgetProposal));
	budgetProposal.Relay();
	if (budget.AddProposal(budgetProposal)) {
		return budgetProposal.GetHash().ToString();
	}
	throw runtime_error("Invalid proposal, see debug.log for details.");
}

Value proposalvote(const Array& params, bool fHelp)
{
	if (fHelp || params.size() != 3)
		throw runtime_error(
			"proposalvote mk \"proposal-hash\" \"yes|no\""
			"\nVote on a budget proposal\n"
			"\nArguments:\n"
			"1. \"voting-address\"      (string, required) Divi address for the voter\n"
			"2. \"proposal-hash\"       (string, required) The identifying hash for the proposal\n"
			"3. \"vote-cast\"           (string, required) 'yes' to vote for the proposal, 'no' to vote against\n"
			"\nResult:\n"
			"\"status\"     (string) Vote status or error message\n"
			"\nExamples:\n" +
			HelpExampleCli("proposalvote", "\"ed2f83cedee59a91406f5f47ec4d60bf5a7f9ee6293913c82976bd2d3a658041\" \"yes\"") +
			HelpExampleRpc("proposalvote", "\"ed2f83cedee59a91406f5f47ec4d60bf5a7f9ee6293913c82976bd2d3a658041\" \"yes\""));

	std::string address = params[0].get_str();
	// ToDo need to switch to address list
	if (mnodeman.Find(address) == NULL) { return "Failure to find masternode in list : " + address; }
	uint256 hash = ParseHashV(params[1], "parameter 1");
	std::string strVote = params[2].get_str();
	if (strVote != "yes" && strVote != "no") throw JSONRPCError(RPC_INVALID_PARAMETER, "You can only vote 'yes' or 'no'");

	CBudgetVote vote({ address, hash, strVote });
	vote.Sign();

	std::string strError = "";
	if (!budget.UpdateProposal(vote, NULL, strError))  throw JSONRPCError(RPC_VERIFY_ERROR, "ERROR: " + strError);
	budget.mapSeenMasternodeBudgetVotes.insert(make_pair(vote.GetHash(), vote));
	vote.Relay();
	return "Voted successfully!";
}

Value proposalrawvote(const Array& params, bool fHelp)
{
	if (fHelp || params.size() != 5)
		throw runtime_error(
			"mnbudgetrawvote \"voting-address\" \"proposal-hash\" yes|no time \"vote-sig\"\n"
			"\nCompile and relay a proposal vote with provided external signature instead of signing vote internally\n"
			"\nArguments:\n"
			"1. \"voting-address\"      (string, required) Divi address for the voter\n"
			"2. \"proposal-hash\"       (string, required) The identifying hash for the proposal\n"
			"3. \"vote-cast\"           (string, required) 'yes' to vote for the proposal, 'no' to vote against\n"
			"4. time                  (numeric, required) Time vote was signed\n"
			"5. \"vote-sig\"            (string, required) External signature\n"
			"\nResult:\n"
			"\"status\"     (string) Vote status or error message\n"
			"\nExamples:\n" +
			HelpExampleCli("mnproposalrawvote", "") + HelpExampleRpc("mnproposalrawvote", ""));

	std::string address = params[0].get_str();
	// ToDo need to switch to address list
	if (mnodeman.Find(address) == NULL) { return "Failure to find masternode in list : " + address; }
	uint256 hash = ParseHashV(params[1], "parameter 1");
	std::string strVote = params[2].get_str();
	if (strVote != "yes" && strVote != "no") throw JSONRPCError(RPC_INVALID_PARAMETER, "You can only vote 'yes' or 'no'");

	int64_t nTime = params[3].get_int64();
	std::string strSig = params[4].get_str();
		bool fInvalid = false;
	std::vector<unsigned char> vchSig = DecodeBase64(strSig.c_str(), &fInvalid);
		if (fInvalid) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Bad signature: Malformed base64 encoding");
	CBudgetVote vote({ address, hash, strVote, nTime, vchSig });
	if (!obfuScationSigner.VerifyMessage(address, vote.ToString(), vchSig)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Failed to verify signature.");

	std::string strError = "";
	if (!budget.UpdateProposal(vote, NULL, strError))  throw JSONRPCError(RPC_VERIFY_ERROR, "ERROR: " + strError);
	budget.mapSeenMasternodeBudgetVotes.insert(make_pair(vote.GetHash(), vote));
	vote.Relay();
	return "Voted successfully!";
}

Value getproposalvotes(const Array& params, bool fHelp)
{
	if (params.size() != 1)
		throw runtime_error(
			"getbudgetvotes \"proposal-name\"\n"
			"\nPrint vote information for a budget proposal\n"

			"\nArguments:\n"
			"1. \"proposal-name\":      (string, required) Name of the proposal\n"

			"\nResult:\n"
			"[\n"
			"  {\n"
			"    \"mnId\": \"xxxx\",        (string) Hash of the masternode's collateral transaction\n"
			"    \"nHash\": \"xxxx\",       (string) Hash of the vote\n"
			"    \"Vote\": \"YES|NO\",      (string) Vote cast ('YES' or 'NO')\n"
			"    \"nTime\": xxxx,         (numeric) Time in seconds since epoch the vote was cast\n"
			"  }\n"
			"  ,...\n"
			"]\n"
			"\nExamples:\n" +
			HelpExampleCli("getbudgetvotes", "\"test-proposal\"") + HelpExampleRpc("getbudgetvotes", "\"test-proposal\""));

	std::string strProposalName = SanitizeString(params[0].get_str());

	Array ret;

	CBudgetProposal* pbudgetProposal = budget.FindProposal(strProposalName);

	if (pbudgetProposal == NULL) throw runtime_error("Unknown proposal name");

	std::map<uint256, CBudgetVote>::iterator it = pbudgetProposal->mapVotes.begin();
	while (it != pbudgetProposal->mapVotes.end()) {
		Object bObj;
		bObj.push_back(Pair("mnId", (*it).second.address));
		bObj.push_back(Pair("nHash", (*it).first.ToString().c_str()));
		bObj.push_back(Pair("Vote", (*it).second.GetVoteString()));
		bObj.push_back(Pair("nTime", (int64_t)(*it).second.nTime));
		ret.push_back(bObj);
		it++;
	}
	return ret;
}

Value getnextsuperblock(const Array& params, bool fHelp)
{
	if (fHelp || params.size() != 0)
		throw runtime_error(
			"getnextsuperblock\n"
			"\nPrint the next super block height\n"

			"\nResult:\n"
			"n      (numeric) Block height of the next super block\n"
			"\nExamples:\n" +
			HelpExampleCli("getnextsuperblock", "") + HelpExampleRpc("getnextsuperblock", ""));

	CBlockIndex* pindexPrev = chainActive.Tip();
	if (!pindexPrev) return "unknown";

	int nNext = pindexPrev->nHeight - pindexPrev->nHeight % GetBudgetPaymentCycleBlocks() + GetBudgetPaymentCycleBlocks();
	return nNext;
}

Value getbudgetprojection(const Array& params, bool fHelp)
{
	if (fHelp || params.size() != 0)
		throw runtime_error(
			"getbudgetprojection\n"
			"\nShow the projection of which proposals will be paid the next cycle\n"

			"\nResult:\n"
			"[\n"
			"  {\n"
			"    \"Name\": \"xxxx\",               (string) Proposal Name\n"
			"    \"URL\": \"xxxx\",                (string) Proposal URL\n"
			"    \"Hash\": \"xxxx\",               (string) Proposal vote hash\n"
			"    \"FeeHash\": \"xxxx\",            (string) Proposal fee hash\n"
			"    \"BlockStart\": n,              (numeric) Proposal starting block\n"
			"    \"BlockEnd\": n,                (numeric) Proposal ending block\n"
			"    \"TotalPaymentCount\": n,       (numeric) Number of payments\n"
			"    \"RemainingPaymentCount\": n,   (numeric) Number of remaining payments\n"
			"    \"PaymentAddress\": \"xxxx\",     (string) DIVI address of payment\n"
			"    \"Ratio\": x.xxx,               (numeric) Ratio of yeas vs nays\n"
			"    \"Yeas\": n,                    (numeric) Number of yea votes\n"
			"    \"Nays\": n,                    (numeric) Number of nay votes\n"
			"    \"Abstains\": n,                (numeric) Number of abstains\n"
			"    \"TotalPayment\": xxx.xxx,      (numeric) Total payment amount\n"
			"    \"MonthlyPayment\": xxx.xxx,    (numeric) Monthly payment amount\n"
			"    \"IsEstablished\": true|false,  (boolean) Established (true) or (false)\n"
			"    \"IsValid\": true|false,        (boolean) Valid (true) or Invalid (false)\n"
			"    \"IsValidReason\": \"xxxx\",      (string) Error message, if any\n"
			"    \"Alloted\": xxx.xxx,           (numeric) Amount alloted in current period\n"
			"    \"TotalBudgetAlloted\": xxx.xxx (numeric) Total alloted\n"
			"  }\n"
			"  ,...\n"
			"]\n"
			"\nExamples:\n" +
			HelpExampleCli("getbudgetprojection", "") + HelpExampleRpc("getbudgetprojection", ""));

	Array ret;
	Object resultObj;
	CAmount nTotalAllotted = 0;

	std::vector<CBudgetProposal*> winningProps = budget.GetBudget();
	BOOST_FOREACH(CBudgetProposal* pbudgetProposal, winningProps) {
		nTotalAllotted += pbudgetProposal->GetAllotted();

		CTxDestination address1;
		ExtractDestination(pbudgetProposal->GetPayee(), address1);
		CBitcoinAddress address2(address1);

		Object bObj;
		budgetToJSON(pbudgetProposal, bObj);
		bObj.push_back(Pair("Alloted", ValueFromAmount(pbudgetProposal->GetAllotted())));
		bObj.push_back(Pair("TotalBudgetAlloted", ValueFromAmount(nTotalAllotted)));

		ret.push_back(bObj);
	}

	return ret;
}

Value getbudgetinfo(const Array& params, bool fHelp)
{
	if (fHelp || params.size() > 1)
		throw runtime_error(
			"getbudgetinfo ( \"proposal\" )\n"
			"\nShow current masternode budgets\n"

			"\nArguments:\n"
			"1. \"proposal\"    (string, optional) Proposal name\n"

			"\nResult:\n"
			"[\n"
			"  {\n"
			"    \"Name\": \"xxxx\",               (string) Proposal Name\n"
			"    \"URL\": \"xxxx\",                (string) Proposal URL\n"
			"    \"Hash\": \"xxxx\",               (string) Proposal vote hash\n"
			"    \"FeeHash\": \"xxxx\",            (string) Proposal fee hash\n"
			"    \"BlockStart\": n,              (numeric) Proposal starting block\n"
			"    \"BlockEnd\": n,                (numeric) Proposal ending block\n"
			"    \"TotalPaymentCount\": n,       (numeric) Number of payments\n"
			"    \"RemainingPaymentCount\": n,   (numeric) Number of remaining payments\n"
			"    \"PaymentAddress\": \"xxxx\",     (string) DIVI address of payment\n"
			"    \"Ratio\": x.xxx,               (numeric) Ratio of yeas vs nays\n"
			"    \"Yeas\": n,                    (numeric) Number of yea votes\n"
			"    \"Nays\": n,                    (numeric) Number of nay votes\n"
			"    \"Abstains\": n,                (numeric) Number of abstains\n"
			"    \"TotalPayment\": xxx.xxx,      (numeric) Total payment amount\n"
			"    \"MonthlyPayment\": xxx.xxx,    (numeric) Monthly payment amount\n"
			"    \"IsEstablished\": true|false,  (boolean) Established (true) or (false)\n"
			"    \"IsValid\": true|false,        (boolean) Valid (true) or Invalid (false)\n"
			"    \"IsValidReason\": \"xxxx\",      (string) Error message, if any\n"
			"  }\n"
			"  ,...\n"
			"]\n"
			"\nExamples:\n" +
			HelpExampleCli("getbudgetprojection", "") + HelpExampleRpc("getbudgetprojection", ""));

	Array ret;

	std::string strShow = "valid";
	if (params.size() == 1) {
		std::string strProposalName = SanitizeString(params[0].get_str());
		CBudgetProposal* pbudgetProposal = budget.FindProposal(strProposalName);
		if (pbudgetProposal == NULL) throw runtime_error("Unknown proposal name");
		Object bObj;
		budgetToJSON(pbudgetProposal, bObj);
		ret.push_back(bObj);
		return ret;
	}

	std::vector<CBudgetProposal*> winningProps = budget.GetAllProposals();
	BOOST_FOREACH(CBudgetProposal* pbudgetProposal, winningProps) {
		Object bObj;
		budgetToJSON(pbudgetProposal, bObj);
		ret.push_back(bObj);
	}
	return ret;
}


Value checkbudgets(const Array& params, bool fHelp)
{
	throw runtime_error("Manual budget check initiation is a thing of the past");
}
