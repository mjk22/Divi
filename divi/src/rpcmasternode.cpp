// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2015-2017 The PIVX Developers
// Copyright (c) 2017-2018 The Divi Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// ToDo: add optional changeAddress to all transfers

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

#include <boost/tokenizer.hpp>

#include <fstream>
using namespace json_spirit;

Value debug(const Array& params, bool fHelp)
{
	Object obj;

	// obj.push_back(Pair("pubKey", pubkey.GetHash().GetHex()));

	return obj;
}

Value allocatefunds(const Array& params, bool fHelp)
{
	if (fHelp || params.size() != 2)
		throw runtime_error(
			"allocatefunds purpose amount ( \"pay wallet\" ( \"voting wallet\" ) )\n"
			"\nStarts escrows funds for some purpose.\n"

			"\nArguments:\n"
			"1. purpose			(string, required) helpful identifier to recognize this allocation later.\n"
			"2. amount			(numeric, required) amount of divi funded.\n"

			"\nResult:\n"
			"\"TxId\"			(string) funding transaction id necessary for next step.\n");

	CBitcoinAddress acctAddr = GetAccountAddress("alloc->" + params[0].get_str());
	CAmount nAmount = AmountFromValue(params[1]);
	CWalletTx wtx;
	SendMoney(acctAddr.Get(), nAmount, wtx);

	Object obj;
	obj.push_back(Pair("TxId", wtx.GetHash().GetHex()));
	return obj;
}

Value fundmasternode(const Array& params, bool fHelp)
{
	if (fHelp || params.size() < 3 || params.size() > 6)
		throw runtime_error(
			"fundmasternode masternode amount ( \"pay wallet\" ( \"voting wallet\" ) )\n"
			"\nEscrows funds for a masternode and returns necessary info for it's configuration file.\n"

			"\nArguments:\n"
			"1. alias			(string, required) helpful identifier to recognize this allocation later.\n"
			"2. funding			(string, required) funding transaction id .\n"
			"3. masternode		(string, required) public key of masternode.\n"
			"4. amount			(numeric, required) amount of divi funded.\n"
			"5. pay wallet		(string, optional) public key of pay wallet (if different from funding wallet).\n"
			"6. voting wallet	(string, optional) public key of voting wallet (if different from pay wallet).\n"
			"(use an empty string for the pay wallet if the same as the funding wallet and you wish to assign a different voting wallet).\n"

			"\nResult:\n"
			"\"your lock config\"		(string) funding transaction details for locking (in your 'masternode configuration file').\n"
			"\"masternode config\"		(string) details and signature proving that you authorized this to give to master node.\n");

	// locate the funding transaction
	uint256 txHash = uint256(params[1].get_str());
	CMnFunding fn = CMnFunding();
	fn.amount = AmountFromValue(params[3]);
	vector<COutput> vCoins;
	pwalletMain->AvailableCoins(vCoins);
	COutput* selectedOutput;
	bool found = false;
	BOOST_FOREACH(COutput& out, vCoins) {
		if (out.tx->GetHash() == txHash && out.tx->vout[out.i].nValue == fn.amount) {
			selectedOutput = &out;
			found = true;
			break;
		}
	}
	if (!found) throw JSONRPCError(RPC_VERIFY_ERROR, "Couldn't find transaction with that funding amount");

	fn.vin = CTxIn(txHash, selectedOutput->i);
	if (!fn.CheckVin()) throw JSONRPCError(RPC_VERIFY_ERROR, "Transaction spent");

	CScript pubScript = selectedOutput->tx->vout[selectedOutput->i].scriptPubKey;
	CTxDestination address1;
	ExtractDestination(pubScript, address1);
	CBitcoinAddress address2(address1);

	// assemble the first return
	Object obj;
	std::string vinStr = params[1].get_str() + " " + std::to_string(selectedOutput->i);
	std::string fConfig = params[0].get_str() + " 0.0.0.0:51472 UNUSED " + vinStr;
	obj.push_back(Pair("your lock config", fConfig));

	// start assembling the second return
	std::string payAddress, voteAddress;
	if (params.size() < 5) payAddress = ""; else payAddress = params[4].get_str();
	if (payAddress == "") payAddress = address2.ToString();
	if (params.size() < 6) voteAddress = ""; else voteAddress = params[5].get_str();
	if (voteAddress == "") voteAddress = payAddress;
	std::string mnConfig = params[3].get_str() + " " + vinStr + " " + payAddress + " " + voteAddress;
	std::string strMessage = params[2].get_str() + " " + mnConfig;

	// create and verify the signature before adding it to the second return
	std::vector<unsigned char> sig;
	if (!obfuScationSigner.SignMessage(address2.ToString(), strMessage, sig)) throw JSONRPCError(RPC_VERIFY_ERROR, "Can't sign message");
	if (!obfuScationSigner.VerifyMessage(address2.ToString(), strMessage, sig))  throw JSONRPCError(RPC_VERIFY_ERROR, "Can't verify message");
	obj.push_back(Pair("masternode config", mnConfig + " " + EncodeBase64(&sig[0], sig.size())));
	return obj;
}

Value getpoolinfo(const Array& params, bool fHelp)
{
	if (fHelp || params.size() != 0)
		throw runtime_error(
			"getpoolinfo\n"
			"\nReturns anonymous pool-related information\n"

			"\nResult:\n"
			"{\n"
			"  \"current\": \"addr\",    (string) DIVI address of current masternode\n"
			"  \"state\": xxxx,        (string) unknown\n"
			"  \"entries\": xxxx,      (numeric) Number of entries\n"
			"  \"accepted\": xxxx,     (numeric) Number of entries accepted\n"
			"}\n"

			"\nExamples:\n" +
			HelpExampleCli("getpoolinfo", "") + HelpExampleRpc("getpoolinfo", ""));

	Object obj;
	obj.push_back(Pair("current_masternode", mnodeman.GetCurrentMasterNode()->address));
	obj.push_back(Pair("state", obfuScationPool.GetState()));
	obj.push_back(Pair("entries", obfuScationPool.GetEntriesCount()));
	obj.push_back(Pair("entries_accepted", obfuScationPool.GetCountEntriesAccepted()));
	return obj;
}

Value listmasternodes(const Array& params, bool fHelp)
{
	std::string strFilter = "";

	if (params.size() == 1) strFilter = params[0].get_str();

	if (fHelp || (params.size() > 1))
		throw runtime_error(
			"listmasternodes ( \"filter\" )\n"
			"\nGet a ranked list of masternodes\n"

			"\nArguments:\n"
			"1. \"filter\"    (string, optional) Filter search text. Partial match by pubKey, status, or addr.\n"

			"\nResult:\n"
			"[\n"
			"  {\n"
			"    \"rank\": n,           (numeric) Masternode Rank (or 0 if not enabled)\n"
			"    \"address\": \"hex\",    (string) Masternode Address\n"
			"    \"status\": s,         (string) Status (ENABLED/EXPIRED/REMOVE/etc)\n"
			"  \"netaddr\": \"xxxx\",     (string) Masternode network address\n"
			"    \"version\": v,        (numeric) Masternode protocol version\n"
			"    \"lastseen\": ttt,     (numeric) The time in seconds since epoch (Jan 1 1970 GMT) of the last seen\n"
			"    \"activetime\": ttt,   (numeric) The time in seconds since epoch (Jan 1 1970 GMT) masternode has been active\n"
			"    \"lastpaid\": ttt,     (numeric) The time in seconds since epoch (Jan 1 1970 GMT) masternode was last paid\n"
			"  }\n"
			"  ,...\n"
			"]\n"
			"\nExamples:\n" +
			HelpExampleCli("masternodelist", "") + HelpExampleRpc("masternodelist", ""));

	Array ret;
	int nHeight;
	{
		LOCK(cs_main);
		CBlockIndex* pindex = chainActive.Tip();
		if (!pindex) return 0;
		nHeight = pindex->nHeight;
	}
	std::vector<pair<int, CMasternode> > vMasternodeRanks = mnodeman.GetMasternodeRanks(nHeight);
	BOOST_FOREACH(PAIRTYPE(int, CMasternode) &s, vMasternodeRanks) {
		Object obj;
		CMasternode mn = s.second;
		if (strFilter != "" && mn.address.find(strFilter) == string::npos &&
			mn.Status().find(strFilter) == string::npos &&
			mn.address.find(strFilter) == string::npos) continue;

		obj.push_back(Pair("rank", (mn.IsEnabled() ? s.first : 0)));
		obj.push_back(Pair("address", mn.address));
		obj.push_back(Pair("status", mn.Status()));
		obj.push_back(Pair("netaddr", mn.service.ToString()));
		obj.push_back(Pair("version", mn.protocolVersion));
		obj.push_back(Pair("lastseen", (int64_t)mn.lastPing));
		obj.push_back(Pair("activetime", (int64_t)(mn.lastPing - mn.sigTime)));
		obj.push_back(Pair("lastpaid", (int64_t)mn.GetLastPaid()));
		ret.push_back(obj);
	}
	return ret;
}

Value masternodeconnect(const Array& params, bool fHelp)
{
	if (fHelp || (params.size() != 1))
		throw runtime_error(
			"masternodeconnect \"address\"\n"
			"\nAttempts to connect to specified masternode address\n"

			"\nArguments:\n"
			"1. \"address\"     (string, required) IP or net address to connect to\n"

			"\nExamples:\n" +
			HelpExampleCli("masternodeconnect", "\"192.168.0.6:51472\"") + HelpExampleRpc("masternodeconnect", "\"192.168.0.6:51472\""));

	std::string strAddress = params[0].get_str();

	CService addr = CService(strAddress);

	CNode* pnode = ConnectNode((CAddress)addr, NULL, false);
	if (pnode) {
		pnode->Release();
		return Value::null;
	}
	else {
		throw runtime_error("error connecting\n");
	}
}

Value getmasternodecount(const Array& params, bool fHelp)
{
	if (fHelp || (params.size() > 0))
		throw runtime_error(
			"getmasternodecount\n"
			"\nGet masternode count values\n"

			"\nResult:\n"
			"{\n"
			"  \"total\": n,        (numeric) Total masternodes\n"
			"  \"stable\": n,       (numeric) Stable count\n"
			"  \"obfcompat\": n,    (numeric) Obfuscation Compatible\n"
			"  \"enabled\": n,      (numeric) Enabled masternodes\n"
			"  \"inqueue\": n       (numeric) Masternodes in queue\n"
			"}\n"
			"\nExamples:\n" +
			HelpExampleCli("getmasternodecount", "") + HelpExampleRpc("getmasternodecount", ""));

	Object obj;
	int nCount = 0;
	int ipv4 = 0, ipv6 = 0, onion = 0;

	if (chainActive.Tip())
		mnodeman.GetNextMasternodeInQueueForPaymentX(chainActive.Tip()->nHeight, true, nCount);

	mnodeman.CountNetworks(ActiveProtocol(), ipv4, ipv6, onion);

	obj.push_back(Pair("total", mnodeman.size()));
	obj.push_back(Pair("stable", mnodeman.stable_size()));
	obj.push_back(Pair("obfcompat", mnodeman.CountEnabled(ActiveProtocol())));
	obj.push_back(Pair("enabled", mnodeman.CountEnabled()));
	obj.push_back(Pair("inqueue", nCount));
	obj.push_back(Pair("ipv4", ipv4));
	obj.push_back(Pair("ipv6", ipv6));
	obj.push_back(Pair("onion", onion));

	return obj;
}

Value masternodecurrent(const Array& params, bool fHelp)
{
	if (fHelp || (params.size() != 0))
		throw runtime_error(
			"masternodecurrent\n"
			"\nGet current masternode winner\n"

			"\nResult:\n"
			"{\n"
			"  \"protocol\": xxxx,        (numeric) Protocol version\n"
			"  \"address\": \"xxxx\",      (string) MN address\n"
			"  \"lastseen\": xxx,       (numeric) Time since epoch of last seen\n"
			"  \"activeseconds\": xxx,  (numeric) Seconds MN has been active\n"
			"}\n"
			"\nExamples:\n" +
			HelpExampleCli("masternodecurrent", "") + HelpExampleRpc("masternodecurrent", ""));

	CMasternode* winner = mnodeman.GetCurrentMasterNode(1);
	if (winner) {
		Object obj;
		obj.push_back(Pair("protocol", (int64_t)winner->protocolVersion));
		obj.push_back(Pair("address", winner->address));
		obj.push_back(Pair("lastseen", winner->lastPing));
		obj.push_back(Pair("activeseconds", winner->lastPing - winner->sigTime));
		return obj;
	}

	throw runtime_error("unknown");
}

Value startmasternode(const Array& params, bool fHelp)
{
	if (fHelp || params.size() > 1)
		throw runtime_error(
			"startmasternode lockwallet\n"
			"\nAttempts to start one or more masternode(s)\n"
			"\nArguments:\n"
			"1. lockwallet  (boolean, required) Lock wallet after completion.\n"
			"\nResult:\n"
			"\"status\"     (string) Masternode status message\n"
			"\nExamples:\n" +
			HelpExampleCli("startmasternode", "\"0\"") + HelpExampleRpc("startmasternode", "\"0\""));

	if (!fMasterNode) throw JSONRPCError(RPC_MISC_ERROR, "Error: You must set masternode=1 in the configuration.");

	//	bool result = activeMasternode.Register(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), errorMessage);
	if (pwalletMain->IsLocked()) throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

	if (activeMasternode.IsEnabled()) {
		activeMasternode.ManageStatus();
		if (params[0].get_str() == "true") pwalletMain->Lock();
	}

	return activeMasternode.Status();
}

Value masternodedebug(const Array& params, bool fHelp)
{
	throw runtime_error("masternodedebug is no longer used!");
}

Value createmasternodekey(const Array& params, bool fHelp)
{
	throw runtime_error("use getmasternodestatus instead");
}

Value getmasternodeoutputs(const Array& params, bool fHelp)
{
	throw runtime_error("getmasternodeoutputs is no longer used!");
}

Value listmasternodeconf(const Array& params, bool fHelp)
{
	throw runtime_error("listmasternodeconf is no longer used!");
}

Value getmasternodestatus(const Array& params, bool fHelp)
{
	if (fHelp || (params.size() != 0))
		throw runtime_error(
			"getmasternodestatus\n"
			"\nPrint masternode status\n"

			"\nResult:\n"
			"{\n"
			"  \"address\": \"xxxx\",      (string) Masternode address\n"
			"  \"netaddr\": \"xxxx\",     (string) Masternode network address\n"
			"  \"status\": \"xxxx\",      (string) Masternode status\n"
			"}\n"

			"\nExamples:\n" +
			HelpExampleCli("getmasternodestatus", "") + HelpExampleRpc("getmasternodestatus", ""));

	if (!fMasterNode) throw runtime_error("This is not a masternode");
	Object mnObj;
	mnObj.push_back(Pair("address", activeMasternode.address));
	mnObj.push_back(Pair("netaddr", activeMasternode.service.ToString()));
	mnObj.push_back(Pair("status", activeMasternode.Status()));
	return mnObj;
}

Value getmasternodewinners(const Array& params, bool fHelp)
{
	if (fHelp || params.size() > 3)
		throw runtime_error(
			"getmasternodewinners ( blocks \"filter\" )\n"
			"\nPrint the masternode winners for the last n blocks\n"

			"\nArguments:\n"
			"1. blocks      (numeric, optional) Number of previous blocks to show (default: 10)\n"
			"2. filter      (string, optional) Search filter matching MN address\n"

			"\nResult (single winner):\n"
			"[\n"
			"  {\n"
			"    \"nHeight\": n,           (numeric) block height\n"
			"    \"winner\": {\n"
			"      \"address\": \"xxxx\",    (string) DIVI MN Address\n"
			"      \"nVotes\": n,          (numeric) Number of votes for winner\n"
			"    }\n"
			"  }\n"
			"  ,...\n"
			"]\n"

			"\nResult (multiple winners):\n"
			"[\n"
			"  {\n"
			"    \"nHeight\": n,           (numeric) block height\n"
			"    \"winner\": [\n"
			"      {\n"
			"        \"address\": \"xxxx\",  (string) DIVI MN Address\n"
			"        \"nVotes\": n,        (numeric) Number of votes for winner\n"
			"      }\n"
			"      ,...\n"
			"    ]\n"
			"  }\n"
			"  ,...\n"
			"]\n"
			"\nExamples:\n" +
			HelpExampleCli("getmasternodewinners", "") + HelpExampleRpc("getmasternodewinners", ""));

	int nHeight;
	{
		LOCK(cs_main);
		CBlockIndex* pindex = chainActive.Tip();
		if (!pindex) return 0;
		nHeight = pindex->nHeight;
	}

	int nLast = 10;
	std::string strFilter = "";

	if (params.size() >= 1)
		nLast = atoi(params[0].get_str());

	if (params.size() == 2)
		strFilter = params[1].get_str();

	Array ret;

	for (int i = nHeight - nLast; i < nHeight + 20; i++) {
		Object obj;
		obj.push_back(Pair("nHeight", i));

		std::string strPayment = GetRequiredPaymentsString(i);
		if (strFilter != "" && strPayment.find(strFilter) == std::string::npos) continue;

		if (strPayment.find(',') != std::string::npos) {
			Array winner;
			boost::char_separator<char> sep(",");
			boost::tokenizer< boost::char_separator<char> > tokens(strPayment, sep);
			BOOST_FOREACH(const string& t, tokens) {
				Object addr;
				std::size_t pos = t.find(":");
				std::string strAddress = t.substr(0, pos);
				uint64_t nVotes = atoi(t.substr(pos + 1));
				addr.push_back(Pair("address", strAddress));
				addr.push_back(Pair("nVotes", nVotes));
				winner.push_back(addr);
			}
			obj.push_back(Pair("winner", winner));
		}
		else if (strPayment.find("Unknown") == std::string::npos) {
			Object winner;
			std::size_t pos = strPayment.find(":");
			std::string strAddress = strPayment.substr(0, pos);
			uint64_t nVotes = atoi(strPayment.substr(pos + 1));
			winner.push_back(Pair("address", strAddress));
			winner.push_back(Pair("nVotes", nVotes));
			obj.push_back(Pair("winner", winner));
		}
		else {
			Object winner;
			winner.push_back(Pair("address", strPayment));
			winner.push_back(Pair("nVotes", 0));
			obj.push_back(Pair("winner", winner));
		}

		ret.push_back(obj);
	}

	return ret;
}

Value getmasternodescores(const Array& params, bool fHelp)
{
	if (fHelp || params.size() > 1)
		throw runtime_error(
			"getmasternodescores ( blocks )\n"
			"\nPrint list of winning masternode by score\n"

			"\nArguments:\n"
			"1. blocks      (numeric, optional) Show the last n blocks (default 10)\n"

			"\nResult:\n"
			"{\n"
			"  xxxx: \"xxxx\"   (numeric : string) Block height : Masternode hash\n"
			"  ,...\n"
			"}\n"
			"\nExamples:\n" +
			HelpExampleCli("getmasternodescores", "") + HelpExampleRpc("getmasternodescores", ""));

	int nLast = 10;

	if (params.size() == 1) {
		try {
			nLast = std::stoi(params[0].get_str());
		}
		catch (const boost::bad_lexical_cast &) {
			throw runtime_error("Exception on param 2");
		}
	}
	Object obj;

	std::vector<CMasternode> vMasternodes = mnodeman.GetFullMasternodeVector();
	for (int nHeight = chainActive.Tip()->nHeight - nLast; nHeight < chainActive.Tip()->nHeight + 20; nHeight++) {
		uint256 nHigh = 0;
		CMasternode* pBestMasternode = NULL;
		BOOST_FOREACH(CMasternode& mn, vMasternodes) {
			uint256 n = mn.CalculateScore(nHeight - 100);
			if (n > nHigh) {
				nHigh = n;
				pBestMasternode = &mn;
			}
		}
		if (pBestMasternode)
			obj.push_back(Pair(strprintf("%d", nHeight), pBestMasternode->address));
	}

	return obj;
}
