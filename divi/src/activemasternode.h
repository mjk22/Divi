// Copyright (c) 2014-2016 The Dash developers
// Copyright (c) 2015-2017 The PIVX Developers
// Copyright (c) 2017-2018 The Divi Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ACTIVEMASTERNODE_H
#define ACTIVEMASTERNODE_H

#include "init.h"
#include "key.h"
#include "masternode.h"
#include "net.h"
#include "obfuscation.h"
#include "sync.h"
#include "wallet.h"

// Responsible for activating the Masternode and pinging the network
class CActiveMasternode: public CMasternode
{
private:
	CKey keyMasternode;

	bool SendMasternodePingX(std::string& errorMessage);
	bool Register(std::string& errorMessage);

public:
	void ManageStatus();
};

#endif
