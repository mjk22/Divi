// Copyright (c) 2014-2016 The Dash developers
// Copyright (c) 2015-2017 The PIVX Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"
#include "addrman.h"
#include "masternode.h"
#include "masternodeconfig.h"
#include "masternodeman.h"
#include "protocol.h"
#include "spork.h"

//
// Bootup the Masternode, check funding and register on the network
//
void CActiveMasternode::Startup()
{
	if (!fMasterNode) return;

	status = MASTERNODE_NOT_CAPABLE;
	if (pwalletMain->IsLocked()) { LogPrintf("CActiveMasternode::ManageStatus() - not capable: Wallet is locked.\n"); return; }
	if (strMasterNodeAddr.empty()) { LogPrintf("CActiveMasternode::ManageStatus() - not capable: No external address\n"); return; }
	service = CService(strMasterNodeAddr);

	LogPrintf("CActiveMasternode::ManageStatus() - Checking inbound connection to '%s'\n", service.ToString());
	CNode* pnode = ConnectNode((CAddress)service, NULL, false);
	if (!pnode) { LogPrintf("CActiveMasternode::ManageStatus() - not capable: Could not connect to %s\n", service.ToString()); return; }
	pnode->Release();

	if (!VerifyFunding()) { LogPrintf("CActiveMasternode::ManageStatus() - Funding verification failed!\n", notCapableReason); return; }
	if (!Register()) { LogPrintf("Register::ManageStatus() - Error on Register: %s\n", errorMessage); return; }

	status = MASTERNODE_ENABLED;

	if (!SendMasternodePingX(errorMessage)) { LogPrintf("CActiveMasternode::ManageStatus() - Error on Ping: %s\n", errorMessage); }
}

bool CActiveMasternode::SendMasternodePingX(std::string& errorMessage)
{
	CPubKey pubKeyMasternode;
	CKey keyMasternode;

	if (!obfuScationSigner.SetKey(strMasterNodePrivKey, errorMessage, keyMasternode, pubKeyMasternode)) {
		errorMessage = strprintf("Error upon calling SetKey: %s\n", errorMessage);
		return false;
	}

	LogPrintf("CActiveMasternode::SendMasternodePing() - Relay Masternode Ping vin = %s\n", vin.ToString());

	CMasternodePing mnp(vin);
	if (!mnp.Sign(keyMasternode, pubKeyMasternode)) {
		errorMessage = "Couldn't sign Masternode Ping";
		return false;
	}

	// Update lastPing for our masternode in Masternode list
	CMasternode* pmn = mnodeman.Find(vin);
	if (pmn != NULL) {
		if (pmn->IsPingedWithin(MASTERNODE_PING_SECONDS, mnp.sigTime)) {
			errorMessage = "Too early to send Masternode Ping";
			return false;
		}

		pmn->lastPing = &mnp;
		mnodeman.mapSeenMasternodePing.insert(make_pair(mnp.GetHash(), mnp));

		//mnodeman.mapSeenMasternodeBroadcast.lastPing is probably outdated, so we'll update it
		CMasternodeBroadcast mnb(*pmn);
		uint256 hash = mnb.GetHash();
		if (mnodeman.mapSeenMasternodeBroadcast.count(hash)) mnodeman.mapSeenMasternodeBroadcast[hash].lastPing = mnp;

		mnp.Relay();
		return true;
	}
	else {
		// Seems like we are trying to send a ping while the Masternode is not registered in the network
		errorMessage = "Obfuscation Masternode List doesn't include our Masternode, shutting down Masternode pinging service! " + vin.ToString();
		status = MASTERNODE_NOT_CAPABLE;
		return false;
	}
}

bool CActiveMasternode::Register(std::string& errorMessage)
{
	if (!masternodeSync.IsBlockchainSynced()) {	LogPrintf("CActiveMasternode::Register() - MASTERNODE_SYNC_IN_PROCESS\n"); return false; }

	addrman.Add(CAddress(service), CNetAddr("127.0.0.1"), 2 * 60 * 60);

	CMasternodePing mnp = CMasternodePing(address);
	if (!mnp.Sign() { LogPrintf("CActiveMasternode::Register() - Failed to sign ping, address:  %s\n", address.ToString()); return false; }
	mnodeman.mapSeenMasternodePing.insert(make_pair(mnp.GetHash(), mnp));

	LogPrintf("CActiveMasternode::Register() - Adding to Masternode list\n    service: %s\n    address: %s\n", service.ToString(), address.ToString());
	lastPing = &mnp;
	if (!Sign()) { LogPrintf("CActiveMasternode::Register() - Failed to sign broadcast, address = %s\n", address); return false; }
	mnodeman.mapSeenMasternodeBroadcast.insert(make_pair(mnb.GetHash(), mnb));
	masternodeSync.AddedMasternodeList(mnb.GetHash());

	CMasternode* pmn = mnodeman.Find(address);
	if (pmn == NULL) {
		CMasternode mn(mnb);
		mnodeman.Add(mn);
	}
	else {
		pmn->UpdateFromNewBroadcast(mnb);
	}

	//send to all peers
	LogPrintf("CActiveMasternode::Register() - Relay address = %s\n", address.ToString());
	Relay();
	return true;
}

