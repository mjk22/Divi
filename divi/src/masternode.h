// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2017 The PIVX Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_H
#define MASTERNODE_H

#include "base58.h"
#include "key.h"
#include "main.h"
#include "net.h"
#include "sync.h"
#include "timedata.h"
#include "util.h"

#define MASTERNODE_MIN_CONFIRMATIONS 15
#define MASTERNODE_MIN_MNP_SECONDS (10 * 60)
#define MASTERNODE_MIN_MNB_SECONDS (5 * 60)
#define MASTERNODE_PING_SECONDS (5 * 60)
#define MASTERNODE_EXPIRATION_SECONDS (120 * 60)
#define MASTERNODE_REMOVAL_SECONDS (130 * 60)
#define MASTERNODE_CHECK_SECONDS 5

using namespace std;

class CMasternode;
class CMasternodePing;
extern map<int64_t, uint256> mapCacheBlockHashes;

bool GetBlockHash(uint256& hash, int nBlockHeight);

//
// The Masternode Ping Class : Contains a different serialize method for sending pings from masternodes throughout the network
//

class CMasternodePing
{
public:
	std::string address;
	uint256 blockHash;
	int64_t sigTime;
	std::vector<unsigned char> vchSig;

	CMasternodePing();
	CMasternodePing(std::string& newAddress);

	ADD_SERIALIZE_METHODS;

	template <typename Stream, typename Operation>
	inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) 
	{
		READWRITE(address);
		READWRITE(blockHash);
		READWRITE(sigTime);
		READWRITE(vchSig);
	}

	bool CheckAndUpdate(int& nDos, bool fRequireEnabled = true);
	bool Sign();
	void Relay();

	uint256 GetHash()
	{
		CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
		ss << address;
		ss << sigTime;
		return ss.GetHash();
	}

	void swap(CMasternodePing& first, CMasternodePing& second) // nothrow
	{
		// enable ADL (not necessary in our case, but good practice)
		using std::swap;

		// by swapping the members of two classes, the two classes are effectively swapped
		swap(first.address, second.address);
		swap(first.blockHash, second.blockHash);
		swap(first.sigTime, second.sigTime);
		swap(first.vchSig, second.vchSig);
	}

	CMasternodePing& operator=(CMasternodePing from)
	{
		swap(*this, from);
		return *this;
	}
	friend bool operator==(const CMasternodePing& a, const CMasternodePing& b)
	{
		return a.address == b.address && a.blockHash == b.blockHash;
	}
	friend bool operator!=(const CMasternodePing& a, const CMasternodePing& b)
	{
		return !(a == b);
	}
};

//
// The Masternode Class & related subclasses
// 

class CMnTier {
public:
	std::string name;
	CAmount collateral;
	int seesawBasis;

	ADD_SERIALIZE_METHODS;

	template <typename Stream, typename Operation>
	inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
	{
		READWRITE(name);
		READWRITE(collateral);
		READWRITE(seesawBasis);
	}
};

const CMnTier Diamond{ "diamond", 100000 * COIN, 2000 };
const CMnTier Platinum{ "platinum", 30000 * COIN, 2000 };
const CMnTier Gold{ "gold", 10000 * COIN, 2000 };
const CMnTier Silver{ "silver", 3000 * COIN, 2000 };
const CMnTier Copper{ "copper", 1000 * COIN, 2000 };

class CMnFunding
{
public:
	CAmount amount;
	CTxIn vin;
	std::string payAddress;
	std::string voteAddress;
	std::vector<unsigned char> sig;

	ADD_SERIALIZE_METHODS;

	template <typename Stream, typename Operation>
	inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
	{
		READWRITE(amount);
		READWRITE(vin);
		READWRITE(payAddress);
		READWRITE(voteAddress);
		READWRITE(sig);
	}

	bool CheckVin();
};

class CMasternode
{
protected:
	mutable CCriticalSection cs;			// critical section to protect the inner data structures
	int64_t lastTimeChecked;

public:
	enum state {
		MASTERNODE_SYNC_IN_PROCESS,
		MASTERNODE_INPUT_TOO_NEW,
		MASTERNODE_NOT_CAPABLE,
		MASTERNODE_INSUFFICIENT_FUNDS,
		MASTERNODE_VIN_SPENT,
		MASTERNODE_ENABLED,
		MASTERNODE_EXPIRED,
		MASTERNODE_REMOVE,
		MASTERNODE_WATCHDOG_EXPIRED,
		MASTERNODE_POSE_BAN,
		MASTERNODE_POS_ERROR
	};

	std::string address;					// used as the masternode ID
	CMnTier tier;
	std::vector<CMnFunding> funding;
	CService service;
	int status;
	std::vector<unsigned char> vchSig;
	int64_t sigTime;						// mnb message time
	int64_t lastPing;						// last ping time
	int protocolVersion;
	int cacheInputAge;
	int cacheInputAgeBlock;
	bool unitTest;
	bool allowFreeTx;
	int64_t nLastDsq;						//the dsq count from the last dsq broadcast of this node
	int nScanningErrorCount;
	int nLastScanningErrorBlockHeight;
	int64_t score;
	int rank;

	CMasternode();
	CMasternode(const CMasternode& other);

	void swap(CMasternode& first, CMasternode& second) // nothrow
	{
		// enable ADL (not necessary in our case, but good practice)
		using std::swap;

		// by swapping the members of two classes, // the two classes are effectively swapped
		swap(first.address, second.address);
		swap(first.tier, second.tier);
		swap(first.funding, second.funding);
		swap(first.service, second.service);
		swap(first.status, second.status);
		swap(first.vchSig, second.vchSig);
		swap(first.sigTime, second.sigTime);
		swap(first.lastPing, second.lastPing);
		swap(first.protocolVersion, second.protocolVersion);
		swap(first.cacheInputAge, second.cacheInputAge);
		swap(first.cacheInputAgeBlock, second.cacheInputAgeBlock);
		swap(first.unitTest, second.unitTest);
		swap(first.allowFreeTx, second.allowFreeTx);
		swap(first.nLastDsq, second.nLastDsq);
		swap(first.nScanningErrorCount, second.nScanningErrorCount);
		swap(first.nLastScanningErrorBlockHeight, second.nLastScanningErrorBlockHeight);
		swap(first.score, second.score);
		swap(first.rank, second.rank);
	}

	CMasternode& operator=(CMasternode from)
	{
		swap(*this, from);
		return *this;
	}
	friend bool operator==(const CMasternode& a, const CMasternode& b)
	{
		return a.address == b.address;
	}
	friend bool operator!=(const CMasternode& a, const CMasternode& b)
	{
		return !(a.address == b.address);
	}

	uint256 CalculateScore(int64_t nBlockHeight = 0);

	ADD_SERIALIZE_METHODS;

	template <typename Stream, typename Operation>
	inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
	{
		LOCK(cs);

		READWRITE(address);
		READWRITE(tier);
		READWRITE(funding);
		READWRITE(service);
		READWRITE(status);
		READWRITE(vchSig);
		READWRITE(sigTime);
		READWRITE(lastPing);
		READWRITE(protocolVersion);
		READWRITE(cacheInputAge);
		READWRITE(cacheInputAgeBlock);
		READWRITE(unitTest);
		READWRITE(allowFreeTx);
		READWRITE(nLastDsq);
		READWRITE(nScanningErrorCount);
		READWRITE(nLastScanningErrorBlockHeight);
		READWRITE(score);
		READWRITE(rank);
	}

	int64_t SecondsSincePayment();

	bool UpdateFromNewBroadcastX(CMasternode& mnb);

	inline uint64_t SliceHash(uint256& hash, int slice)
	{
		uint64_t n = 0;
		memcpy(&n, &hash + slice * 64, 64);
		return n;
	}

	bool VerifyFunding();
	void Check(bool forceCheck = false);

	bool IsBroadcastedWithin(int seconds)
	{
		return (GetAdjustedTime() - sigTime) < seconds;
	}

	bool IsPingedWithin(int seconds, int64_t now = -1)
	{
		now == -1 ? now = GetAdjustedTime() : now;

		return (lastPing == 0) ? false : now - lastPing < seconds;
	}

	void Disable()
	{
		sigTime = 0;
		lastPing = 0;
	}

	bool IsEnabled()
	{
		return status == MASTERNODE_ENABLED;
	}

	std::string Status() 	{
		switch (status) {
		case CMasternode::MASTERNODE_SYNC_IN_PROCESS: return "Sync in progress. Must wait until sync is complete to start Masternode";
		case CMasternode::MASTERNODE_INPUT_TOO_NEW: return strprintf("Masternode input must have at least %d confirmations", MASTERNODE_MIN_CONFIRMATIONS);
		case CMasternode::MASTERNODE_NOT_CAPABLE: return "Masternode has configuration errors.";
		case CMasternode::MASTERNODE_INSUFFICIENT_FUNDS: return "Masternode does not have sufficient funding forthe chosen tier.";
		case CMasternode::MASTERNODE_VIN_SPENT: return "Some or all of masternode funding has been spent.";
		case CMasternode::MASTERNODE_ENABLED: return "ENABLED";
		case CMasternode::MASTERNODE_EXPIRED: return "EXPIRED";
		case CMasternode::MASTERNODE_REMOVE: return "REMOVE";
		case CMasternode::MASTERNODE_WATCHDOG_EXPIRED: return "WATCHDOG_EXPIRED";
		case CMasternode::MASTERNODE_POSE_BAN: return "POSE_BAN";
		case CMasternode::MASTERNODE_POS_ERROR: return "POS_ERROR";
		default: return "UNKNOWN";
		}
	}

	int64_t GetLastPaid();
	bool IsValidNetAddr();

	// Broadcast Functions
	bool CheckAndUpdate(int& nDoS);
	bool CheckInputsAndAdd(int& nDos);
	bool Sign();
	void Relay();

	uint256 GetHash()
	{
		CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
		ss << sigTime;
		ss << address;
		return ss.GetHash();
	}
};

#endif
