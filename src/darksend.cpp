

#include "darksend.h"
#include "main.h"
#include "init.h"

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/lexical_cast.hpp>

#include <algorithm>
#include <boost/assign/list_of.hpp>

using namespace std;
using namespace boost;


/** The main object for accessing darksend */
CDarkSendPool darkSendPool;
/** A helper object for signing messages from masternodes */
CDarkSendSigner darkSendSigner;
/** Object for who's going to get paid on which blocks */
CMasternodePayments masternodePayments;
/** The list of active masternodes */
std::vector<CMasterNode> darkSendMasterNodes;
/** All denominations used by darksend */
std::vector<int64> darkSendDenominations;
/** Which masternodes we're asked other clients for */
std::vector<CTxIn> vecMasternodeAskedFor;
/** The current darksends in progress on the network */
std::vector<CDarksendQueue> vecDarksendQueue;
// count peers we've requested the list from
int RequestedMasterNodeList = 0;

/* *** BEGIN DARKSEND MAGIC - DARKCOIN **********
    Copyright 2014, Darkcoin Developers 
        eduffield - evan@darkcoin.io
*/

struct CompareValueOnly
{
    bool operator()(const pair<int64, CTxIn>& t1,
                    const pair<int64, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};

struct CompareValueOnly2
{
    bool operator()(const pair<int64, int>& t1,
                    const pair<int64, int>& t2) const
    {
        return t1.first < t2.first;
    }
};

int randomizeList (int i) { return std::rand()%i;}

void CDarkSendPool::SetNull(bool clearEverything){
    finalTransaction.vin.clear();
    finalTransaction.vout.clear();

    entries.clear();

    state = POOL_STATUS_ACCEPTING_ENTRIES;

    lastTimeChanged = GetTimeMillis();

    entriesCount = 0;
    lastEntryAccepted = 0;
    countEntriesAccepted = 0;

    sessionUsers = 0;
    sessionAmount = 0;
    sessionFoundMasternode = false;
    sessionTries = 0;
    vecSessionCollateral.clear();
    txCollateral = CTransaction();

    if(clearEverything){
        myEntries.clear();

        if(fMasterNode){
            sessionID = 1 + (rand() % 999999);
        } else {
            sessionID = 0;
        }
    }

    // -- seed random number generator (used for ordering output lists)
    unsigned int seed = 0;
    RAND_bytes((unsigned char*)&seed, sizeof(seed));
    std::srand(seed);
}

bool CDarkSendPool::SetCollateralAddress(std::string strAddress){
    CBitcoinAddress address;
    if (!address.SetString(strAddress))
    {
        LogPrintf("CDarkSendPool::SetCollateralAddress - Invalid DarkSend collateral address\n");
        return false;
    }
    collateralPubKey.SetDestination(address.Get());
    return true;
}

//
// Unlock coins after Darksend fails or succeeds
//
void CDarkSendPool::UnlockCoins(){
    BOOST_FOREACH(CTxIn v, lockedCoins)
        pwalletMain->UnlockCoin(v.prevout);

    lockedCoins.clear();
}

//
// Check the Darksend progress and send client updates if a masternode
// 
void CDarkSendPool::Check()
{
    if(fDebug) LogPrintf("CDarkSendPool::Check()\n");
    if(fDebug) LogPrintf("CDarkSendPool::Check() - entries count %lu\n", entries.size());
    
    // If entries is full, then move on to the next phase
    if(state == POOL_STATUS_ACCEPTING_ENTRIES && entries.size() >= GetMaxPoolTransactions())
    {
        if(fDebug) LogPrintf("CDarkSendPool::Check() -- ACCEPTING OUTPUTS\n");
        UpdateState(POOL_STATUS_FINALIZE_TRANSACTION);
    }

    // create the finalized transaction for distribution to the clients
    if(state == POOL_STATUS_FINALIZE_TRANSACTION && finalTransaction.vin.empty() && finalTransaction.vout.empty()) {
        if(fDebug) LogPrintf("CDarkSendPool::Check() -- FINALIZE TRANSACTIONS\n");
        UpdateState(POOL_STATUS_SIGNING);

        if (fMasterNode) { 
            // make our new transaction
            CTransaction txNew;
            for(unsigned int i = 0; i < entries.size(); i++){
                BOOST_FOREACH(const CTxOut v, entries[i].vout)
                    txNew.vout.push_back(v);

                BOOST_FOREACH(const CDarkSendEntryVin s, entries[i].sev)
                    txNew.vin.push_back(s.vin);
            }
            // shuffle the outputs for improved anonymity
            std::random_shuffle ( txNew.vout.begin(), txNew.vout.end(), randomizeList);

            if(fDebug) LogPrintf("Transaction 1: %s\n", txNew.ToString().c_str());

            SignFinalTransaction(txNew, NULL);

            // request signatures from clients
            RelayDarkSendFinalTransaction(sessionID, txNew);
        }
    }

    // collect signatures from clients

    // If we have all of the signatures, try to compile the transaction
    if(state == POOL_STATUS_SIGNING && SignaturesComplete()) { 
        if(fDebug) LogPrintf("CDarkSendPool::Check() -- SIGNING\n");            
        UpdateState(POOL_STATUS_TRANSMISSION);

        CWalletTx txNew = CWalletTx(pwalletMain, finalTransaction);

        LOCK2(cs_main, pwalletMain->cs_wallet);
        {
            if (fMasterNode) { //only the main node is master atm                
                if(fDebug) LogPrintf("Transaction 2: %s\n", txNew.ToString().c_str());

                // See if the transaction is valid
                if (!txNew.AcceptToMemoryPool(true, false))
                {
                    LogPrintf("CDarkSendPool::Check() - CommitTransaction : Error: Transaction not valid\n");
                    SetNull();
                    pwalletMain->Lock();

                    // not much we can do in this case
                    UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
                    RelayDarkSendCompletedTransaction(sessionID, true, "Transaction not valid, please try again");
                    return;
                }

                LogPrintf("CDarkSendPool::Check() -- IS MASTER -- TRANSMITTING DARKSEND\n");

                // Broadcast the transaction to the network
                txNew.AddSupportingTransactions();
                txNew.fTimeReceivedIsTxTime = true;                
                txNew.RelayWalletTransaction();

                // Tell the clients it was successful                
                RelayDarkSendCompletedTransaction(sessionID, false, "Transaction Created Successfully");
            }
        }
    }

    // move on to next phase, allow 3 seconds incase the masternode wants to send us anything else
    if((state == POOL_STATUS_TRANSMISSION && fMasterNode) || (state == POOL_STATUS_SIGNING && completedTransaction) ) {
        LogPrintf("CDarkSendPool::Check() -- COMPLETED -- RESETTING \n");
        SetNull(true);
        UnlockCoins();
        if(fMasterNode) RelayDarkSendStatus(darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_RESET);    
        pwalletMain->Lock();
    }

    // reset if we're here for 10 seconds
    if((state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) && GetTimeMillis()-lastTimeChanged >= 10000) {
        LogPrintf("CDarkSendPool::Check() -- RESETTING MESSAGE \n");
        SetNull(true);
        if(fMasterNode) RelayDarkSendStatus(darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_RESET);    
        UnlockCoins();
    }
}

//
// Charge clients a fee if they're abusive
//
// Why bother? Darksend uses collateral to ensure abuse to the process is kept to a minimum. 
// The submission and signing stages in darksend are completely separate. In the cases where 
// a client submits a transaction then refused to sign, there must be a cost. Otherwise they
// would be able to do this over and over again and bring the mixing to a hault.
// 
// How does this work? Messages to masternodes come in via "dsi", these require a valid collateral 
// transaction for the client to be able to enter the pool. This transaction is kept by the masternode
// until the transaction is either complete or fails. 
//
void CDarkSendPool::ChargeFees(){
    
    if(fMasterNode) {
        int i = 0;

        if(state == POOL_STATUS_ACCEPTING_ENTRIES){
            BOOST_FOREACH(const CTransaction& txCollateral, vecSessionCollateral) {
                bool found = false;
                BOOST_FOREACH(const CDarkSendEntry& v, entries) {
                    if(v.collateral == txCollateral) {
                        found = true;
                    }
                }

                // This queue entry didn't send us the promised transaction
                if(!found){
                    LogPrintf("CDarkSendPool::ChargeFees -- found uncooperative node (didn't send transaction). charging fees. %u\n", i);

                    CWalletTx wtxCollateral = CWalletTx(pwalletMain, txCollateral);

                    // Broadcast
                    if (!wtxCollateral.AcceptToMemoryPool(true, false))
                    {
                        // This must not fail. The transaction has already been signed and recorded.
                        LogPrintf("CDarkSendPool::ChargeFees() : Error: Transaction not valid");
                    }
                    wtxCollateral.RelayWalletTransaction();
                    i++;
                }
            }
        }

        if(state == POOL_STATUS_SIGNING) {
            // who didn't sign?
            BOOST_FOREACH(const CDarkSendEntry v, entries) {
                BOOST_FOREACH(const CDarkSendEntryVin s, v.sev) {
                    if(!s.isSigSet){
                        LogPrintf("CDarkSendPool::ChargeFees -- found uncooperative node (didn't sign). charging fees. %u\n", i);

                        CWalletTx wtxCollateral = CWalletTx(pwalletMain, v.collateral);

                        // Broadcast
                        if (!wtxCollateral.AcceptToMemoryPool(true, false))
                        {
                            // This must not fail. The transaction has already been signed and recorded.
                            LogPrintf("CDarkSendPool::ChargeFees() : Error: Transaction not valid");
                        }
                        wtxCollateral.RelayWalletTransaction();
                    }
                    i++;
                }
            }
        }
    }
}

//
// Check for various timeouts (queue objects, darksend, etc)
//
void CDarkSendPool::CheckTimeout(){
    // catching hanging sessions
    if(!fMasterNode) {
        if(state == POOL_STATUS_TRANSMISSION) {
            if(fDebug) LogPrintf("CDarkSendPool::CheckTimeout() -- Session complete -- Running Check()\n");
            Check();
        }        
    }

    // check darksend queue objects for timeouts
    int c = 0;
    vector<CDarksendQueue>::iterator it;
    for(it=vecDarksendQueue.begin();it<vecDarksendQueue.end();it++){
        if((*it).IsExpired()){
            if(fDebug) LogPrintf("CDarkSendPool::CheckTimeout() : Removing expired queue entry - %d\n", c);
            vecDarksendQueue.erase(it);
            break;
        }
        c++;
    }

    /* Check to see if we're ready for submissions from clients */
    if(state == POOL_STATUS_QUEUE && sessionUsers == GetMaxPoolTransactions()) {
        CDarksendQueue dsq;
        dsq.nDenom = GetDenominationsByAmount(sessionAmount);
        dsq.vin = vinMasterNode;
        dsq.time = GetTime();
        dsq.ready = true;
        dsq.Sign();
        dsq.Relay();

        UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
    }

    int addLagTime = 0;
    if(!fMasterNode) addLagTime = 10000; //if we're the client, give the server a few extra seconds before resetting.

    if(state == POOL_STATUS_ACCEPTING_ENTRIES || state == POOL_STATUS_QUEUE){
        c = 0;

        // if it's a masternode, the entries are stored in "entries", otherwise they're stored in myEntries
        std::vector<CDarkSendEntry> *vec = &myEntries;
        if(fMasterNode) vec = &entries; 

        // check for a timeout and reset if needed
        vector<CDarkSendEntry>::iterator it2;
        for(it2=vec->begin();it2<vec->end();it2++){
            if((*it2).IsExpired()){
                if(fDebug) LogPrintf("CDarkSendPool::CheckTimeout() : Removing expired entry - %d\n", c);
                vec->erase(it2);
                if(entries.size() == 0 && myEntries.size() == 0){
                    SetNull(true);
                    UnlockCoins();
                }
                if(fMasterNode){
                    RelayDarkSendStatus(darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_RESET);   
                }
                break;
            }
            c++;
        }

        if(GetTimeMillis()-lastTimeChanged >= 30000+addLagTime){
            lastTimeChanged = GetTimeMillis();

            ChargeFees();  
            // reset session information for the queue query stage (before entering a masternode, clients will send a queue request to make sure they're compatible denomination wise)
            sessionUsers = 0;
            sessionAmount = 0;
            sessionFoundMasternode = false;
            sessionTries = 0;            
            vecSessionCollateral.clear();

            UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
        }
    } else if(GetTimeMillis()-lastTimeChanged >= 30000+addLagTime){
        if(fDebug) LogPrintf("CDarkSendPool::CheckTimeout() -- Session timed out (30s) -- resetting\n");
        SetNull();
        UnlockCoins();

        UpdateState(POOL_STATUS_ERROR);
        lastMessage = "Session timed out (30), please resubmit";
    }


    if(state == POOL_STATUS_SIGNING && GetTimeMillis()-lastTimeChanged >= 10000+addLagTime ) {
        if(fDebug) LogPrintf("CDarkSendPool::CheckTimeout() -- Session timed out -- restting\n");
        ChargeFees();
        SetNull();
        UnlockCoins();
        //add my transactions to the new session

        UpdateState(POOL_STATUS_ERROR);
        lastMessage = "Signing timed out, please resubmit";
    }
}

// check to see if the signature is valid
bool CDarkSendPool::SignatureValid(const CScript& newSig, const CTxIn& newVin){
    CTransaction txNew;
    txNew.vin.clear();
    txNew.vout.clear();

    int found = -1;
    CScript sigPubKey = CScript();
    unsigned int i = 0;

    BOOST_FOREACH(CDarkSendEntry e, entries) {
        BOOST_FOREACH(const CTxOut out, e.vout)
            txNew.vout.push_back(out);

        BOOST_FOREACH(const CDarkSendEntryVin s, e.sev){
            txNew.vin.push_back(s.vin);

            if(s.vin == newVin){
                found = i;
                sigPubKey = s.vin.prevPubKey;
            }
            i++;
        }
    }

    if(found >= 0){ //might have to do this one input at a time?
        int n = found;
        txNew.vin[n].scriptSig = newSig;
        if(fDebug) LogPrintf("CDarkSendPool::SignatureValid() - Sign with sig %s\n", newSig.ToString().substr(0,24).c_str());
        if (!VerifyScript(txNew.vin[n].scriptSig, sigPubKey, txNew, n, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC, 0)){
            if(fDebug) LogPrintf("CDarkSendPool::SignatureValid() - Signing - Error signing input %u\n", n);
            return false;
        }
    }

    if(fDebug) LogPrintf("CDarkSendPool::SignatureValid() - Signing - Succesfully signed input\n");
    return true;
}

// check to make sure the collateral provided by the client is valid
bool CDarkSendPool::IsCollateralValid(const CTransaction& txCollateral){
    if(txCollateral.vout.size() < 1) return false;

    int64 nValueIn = 0;
    int64 nValueOut = 0;
    bool missingTx = false;

    CTransaction tx;
    BOOST_FOREACH(const CTxOut o, txCollateral.vout)
        nValueOut += o.nValue;

    BOOST_FOREACH(const CTxIn i, txCollateral.vin){
        CTransaction tx2;
        uint256 hash;
        if(GetTransaction(i.prevout.hash, tx2, hash, true)){
            if(tx2.vout.size() > i.prevout.n) {
                nValueIn += tx2.vout[i.prevout.n].nValue;    
            }
        } else{
            missingTx = true;
        }
    }

    if(missingTx){
        if(fDebug) LogPrintf ("CDarkSendPool::IsCollateralValid - Unknown inputs in collateral transaction - %s\n", txCollateral.ToString().c_str());
        return false; 
    }

    //collateral transactions are required to pay out DARKSEND_COLLATERAL as a fee to the miners
    if(nValueIn-nValueOut < DARKSEND_COLLATERAL) {
        if(fDebug) LogPrintf ("CDarkSendPool::IsCollateralValid - did not include enough fees in transaction %"PRI64d"\n%s\n", nValueOut-nValueIn, txCollateral.ToString().c_str());
        return false;
    }

    LogPrintf("CDarkSendPool::IsCollateralValid %s\n", txCollateral.ToString().c_str());

    CWalletTx wtxCollateral = CWalletTx(pwalletMain, txCollateral);
    if (!wtxCollateral.IsAcceptable(true, false)){
        if(fDebug) LogPrintf ("CDarkSendPool::IsCollateralValid - didn't pass IsAcceptable\n");
        return false;
    }

    return true;
}

// 
// Add a clients transaction to the pool
//
bool CDarkSendPool::AddEntry(const std::vector<CTxIn>& newInput, const int64& nAmount, const CTransaction& txCollateral, const std::vector<CTxOut>& newOutput, std::string& error){
    if (!fMasterNode) return false;

    BOOST_FOREACH(CTxIn in, newInput) {
        if (in.prevout.IsNull() || nAmount < 0) {
            if(fDebug) LogPrintf ("CDarkSendPool::AddEntry - input not valid!\n");
            error = "input not valid";
            sessionUsers--;
            return false;
        }
    }

    if (!IsCollateralValid(txCollateral)){
        if(fDebug) LogPrintf ("CDarkSendPool::AddEntry - collateral not valid!\n");
        error = "collateral not valid";
        sessionUsers--;
        return false;
    }

    if(entries.size() >= GetMaxPoolTransactions()){
        if(fDebug) LogPrintf ("CDarkSendPool::AddEntry - entries is full!\n");   
        error = "entries is full";
        sessionUsers--;
        return false;
    }

    BOOST_FOREACH(CTxIn in, newInput) {
        LogPrintf("looking for vin -- %s\n", in.ToString().c_str());
        BOOST_FOREACH(const CDarkSendEntry v, entries) {
            BOOST_FOREACH(const CDarkSendEntryVin s, v.sev){
                if(s.vin == in) {
                    if(fDebug) LogPrintf ("CDarkSendPool::AddEntry - found in vin\n"); 
                    error = "already have that vin";
                    sessionUsers--;
                    return false;
                }
            }
        }
    }

    if(state == POOL_STATUS_ACCEPTING_ENTRIES) {
        CDarkSendEntry v;
        v.Add(newInput, nAmount, txCollateral, newOutput);
        entries.push_back(v);

        LogPrintf("CDarkSendPool::AddEntry -- adding %s\n", newInput[0].ToString().c_str());
        error = "";

        return true;
    }

    if(fDebug) LogPrintf ("CDarkSendPool::AddEntry - can't accept new entry, wrong state!\n");
    error = "wrong state";
    sessionUsers--;
    return false;
}

bool CDarkSendPool::AddScriptSig(const CTxIn& newVin){
    if(fDebug) LogPrintf("CDarkSendPool::AddScriptSig -- new sig  %s\n", newVin.scriptSig.ToString().substr(0,24).c_str());
    
    BOOST_FOREACH(const CDarkSendEntry v, entries) {
        BOOST_FOREACH(const CDarkSendEntryVin s, v.sev){
            if(s.vin.scriptSig == newVin.scriptSig) {
                LogPrintf("CDarkSendPool::AddScriptSig - already exists \n");
                return false;
            }
        }
    }

    if(!SignatureValid(newVin.scriptSig, newVin)){
        if(fDebug) LogPrintf("CDarkSendPool::AddScriptSig - Invalid Sig\n");
        return false;
    }

    if(fDebug) LogPrintf("CDarkSendPool::AddScriptSig -- sig %s\n", newVin.ToString().c_str());

    if(state == POOL_STATUS_SIGNING) {
        BOOST_FOREACH(CTxIn& vin, finalTransaction.vin){
            if(newVin.prevout == vin.prevout && vin.nSequence == newVin.nSequence){
                vin.scriptSig = newVin.scriptSig;
                vin.prevPubKey = newVin.prevPubKey;
                if(fDebug) LogPrintf("CDarkSendPool::AddScriptSig -- adding to finalTransaction  %s\n", newVin.scriptSig.ToString().substr(0,24).c_str());
            }
        }
        for(unsigned int i = 0; i < entries.size(); i++){
            if(entries[i].AddSig(newVin)){
                if(fDebug) LogPrintf("CDarkSendPool::AddScriptSig -- adding  %s\n", newVin.scriptSig.ToString().substr(0,24).c_str());
                return true;
            }
        }
    }

    LogPrintf("CDarkSendPool::AddScriptSig -- Couldn't set sig!\n" );
    return false;
}

// check to make sure everything is signed
bool CDarkSendPool::SignaturesComplete(){
    BOOST_FOREACH(const CDarkSendEntry v, entries) {
        BOOST_FOREACH(const CDarkSendEntryVin s, v.sev){
            if(!s.isSigSet) return false;
        }
    }
    return true;
}

//
// Execute a darksend denomination via a masternode.
// This is only ran from clients
// 
void CDarkSendPool::SendDarksendDenominate(std::vector<CTxIn>& vin, std::vector<CTxOut>& vout, int64& fee, int64 amount){
    if(darkSendPool.txCollateral == CTransaction()){
        LogPrintf ("CDarksendPool:SendDarksendDenominate() - Darksend collateral not set");
        return;
    }

    // lock the funds we're going to use
    BOOST_FOREACH(CTxIn in, txCollateral.vin)
        lockedCoins.push_back(in);
    
    BOOST_FOREACH(CTxIn in, vin)
        lockedCoins.push_back(in);


    // we should already be connected to a masternode
    if(!sessionFoundMasternode){
        LogPrintf("CDarkSendPool::SendDarksendDenominate() - No masternode has been selected yet.\n");
        UnlockCoins();
        SetNull(true);
        return;
    }

    if (!CheckDiskSpace())
        return;

    if(fMasterNode) {
        LogPrintf("CDarkSendPool::SendDarksendDenominate() - DarkSend from a masternode is not supported currently.\n");
        return;
    }

    UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);

    LogPrintf("CDarkSendPool::SendDarksendDenominate() - Added transaction to pool.\n");

    ClearLastMessage();

    //check it against the memory pool to make sure it's valid
    {
        int64 nValueOut = 0;

        CValidationState state;
        CTransaction tx;

        BOOST_FOREACH(const CTxOut o, vout){
            nValueOut += o.nValue;
            tx.vout.push_back(o);
        }

        BOOST_FOREACH(const CTxIn i, vin){
            tx.vin.push_back(i);

            LogPrintf("dsi -- tx in %s\n", i.ToString().c_str());                
        }


        bool missing = false;
        if (!tx.IsAcceptable(state, true, false, &missing, false)){ //AcceptableInputs(state, true)){
            LogPrintf("dsi -- transactione not valid! %s \n", tx.ToString().c_str());
            return;
        }

        printf("CDarksendPool::SendDarksendDenominate() - preparing transaction - \n %s\n", tx.ToString().c_str());
    }

    // store our entry for later use
    CDarkSendEntry e;
    e.Add(vin, amount, txCollateral, vout);
    myEntries.push_back(e);

    // relay our entry to the master node
    RelayDarkSendIn(vin, amount, txCollateral, vout);
    Check();
}

// Incoming message from masternode updating the progress of darksend
//    newAccepted:  -1 mean's it'n not a "transaction accepted/not accepted" message, just a standard update
//                  0 means transaction was not accepted
//                  1 means transaction was accepted

bool CDarkSendPool::StatusUpdate(int newState, int newEntriesCount, int newAccepted, std::string& error, int newSessionID){
    if(fMasterNode) return false;
    if(state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) return false;

    UpdateState(newState);
    entriesCount = newEntriesCount;

    if(newAccepted != -1) {
        lastEntryAccepted = newAccepted;
        countEntriesAccepted += newAccepted;
        if(newAccepted == 0){
            UpdateState(POOL_STATUS_ERROR);
            lastMessage = error;
        }

        if(newAccepted == 1) {
            sessionID = newSessionID;
            LogPrintf("CDarkSendPool::StatusUpdate - set sessionID to %d\n", sessionID);
            sessionFoundMasternode = true;
        }
    }

    if(newState == POOL_STATUS_ACCEPTING_ENTRIES){
        if(newAccepted == 1){
            LogPrintf("CDarkSendPool::StatusUpdate - entry accepted! \n");
            sessionFoundMasternode = true;
            //wait for other users. Masternode will report when ready
            UpdateState(POOL_STATUS_QUEUE);
        } else if (newAccepted == 0 && sessionID == 0 && !sessionFoundMasternode) {
            LogPrintf("CDarkSendPool::StatusUpdate - entry not accepted by masternode \n");
            UnlockCoins();
            DoAutomaticDenominating(); //try another masternode
        }
        if(sessionFoundMasternode) return true;
    }

    return true;
}

// 
// After we receive the finalized transaction from the masternode, we must 
// check it to make sure it's what we want, then sign it if we agree. 
// If we refuse to sign, it's possible we'll be charged collateral
//
bool CDarkSendPool::SignFinalTransaction(CTransaction& finalTransactionNew, CNode* node){
    if(fDebug) LogPrintf("CDarkSendPool::AddFinalTransaction - Got Finalized Transaction\n");

    if(!finalTransaction.vin.empty()){
        LogPrintf("CDarkSendPool::AddFinalTransaction - Rejected Final Transaction!\n");
        return false;
    }

    finalTransaction = finalTransactionNew;
    LogPrintf("CDarkSendPool::SignFinalTransaction %s\n", finalTransaction.ToString().c_str());
    
    vector<CTxIn> sigs;

    //make sure my inputs/outputs are present, otherwise refuse to sign
    BOOST_FOREACH(const CDarkSendEntry e, myEntries) {
        BOOST_FOREACH(const CDarkSendEntryVin s, e.sev) {
            /* Sign my transaction and all outputs */
            int mine = -1;
            CScript prevPubKey = CScript();
            CTxIn vin = CTxIn();
            
            for(unsigned int i = 0; i < finalTransaction.vin.size(); i++){
                if(finalTransaction.vin[i] == s.vin){
                    mine = i;
                    prevPubKey = s.vin.prevPubKey;
                    vin = s.vin;
                }
            }

            if(mine >= 0){ //might have to do this one input at a time?
                int foundOutputs = 0;
                int64 nValue1 = 0;
                int64 nValue2 = 0;
                
                for(unsigned int i = 0; i < finalTransaction.vout.size(); i++){
                    BOOST_FOREACH(const CTxOut o, e.vout) {
                        if(finalTransaction.vout[i] == o){
                            foundOutputs++;
                            nValue1 += finalTransaction.vout[i].nValue;
                        }
                    }
                }
                
                BOOST_FOREACH(const CTxOut o, e.vout)
                    nValue2 += o.nValue;

                int targetOuputs = e.vout.size();
                if(foundOutputs < targetOuputs || nValue1 != nValue2) {
                    // in this case, something went wrong and we'll refuse to sign. It's possible we'll be charged collateral. But that's 
                    // better then signing if the transaction doesn't look like what we wanted.
                    LogPrintf("CDarkSendPool::Sign - My entries are not correct! Refusing to sign. %d entries %d target. \n", foundOutputs, targetOuputs);
                    return false;
                }

                if(fDebug) LogPrintf("CDarkSendPool::Sign - Signing my input %i\n", mine);
                if(!SignSignature(*pwalletMain, prevPubKey, finalTransaction, mine, int(SIGHASH_ALL|SIGHASH_ANYONECANPAY))) { // changes scriptSig
                    if(fDebug) LogPrintf("CDarkSendPool::Sign - Unable to sign my own transaction! \n");
                    // not sure what to do here, it will timeout...?
                }

                sigs.push_back(finalTransaction.vin[mine]);
                if(fDebug) LogPrintf(" -- dss %d %d %s\n", mine, (int)sigs.size(), finalTransaction.vin[mine].scriptSig.ToString().c_str());
            }
            
        }
        
        if(fDebug) LogPrintf("CDarkSendPool::Sign - txNew:\n%s", finalTransaction.ToString().c_str());
    }

    // push all of our signatures to the masternode
    if(sigs.size() > 0 && node != NULL)
        node->PushMessage("dss", sigs);

    return true;
}

// manage the masternode connections
void CDarkSendPool::ProcessMasternodeConnections(){
    LOCK(cs_vNodes);
    
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        //if it's our masternode, let it be
        if(submittedToMasternode == pnode->addr) continue;

        if(pnode->fDarkSendMaster){
            LogPrintf("Closing masternode connection %s \n", pnode->addr.ToString().c_str());
            pnode->CloseSocketDisconnect();
        }
    }
}

bool CDarkSendPool::ConnectToBestMasterNode(int depth){
    if(fMasterNode) return false;
    
    int winner = GetCurrentMasterNode();
    LogPrintf("winner %d\n", winner);

    if(winner >= 0) {
        LogPrintf("CDarkSendPool::ConnectToBestMasterNode - Connecting to masternode at %s\n", darkSendMasterNodes[winner].addr.ToString().c_str());
        if(ConnectNode((CAddress)darkSendMasterNodes[winner].addr, NULL, true)){
            masterNodeAddr = darkSendMasterNodes[winner].addr.ToString();
            UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
            GetLastValidBlockHash(masterNodeBlockHash);
            return true;
        } else {
            darkSendMasterNodes[winner].enabled = 0;
            if(depth < 5){
                UpdateState(POOL_STATUS_ERROR);
                lastMessage = "Trying MasterNode #" + to_string(depth);
                return ConnectToBestMasterNode(depth+1);
            } else {
                UpdateState(POOL_STATUS_ERROR);
                lastMessage = "No valid MasterNode";
                LogPrintf("CDarkSendPool::ConnectToBestMasterNode - ERROR: %s\n", lastMessage.c_str());
            }
        }
    } else {
        UpdateState(POOL_STATUS_ERROR);
        lastMessage = "No valid MasterNode";
        LogPrintf("CDarkSendPool::ConnectToBestMasterNode - ERROR: %s\n", lastMessage.c_str());
    }

    //failed, so unlock any coins.
    UnlockCoins();

    //if couldn't connect, disable that one and try next
    return false;
}

bool CDarkSendPool::GetMasterNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey)
{
    int64 nValueIn = 0;
    CScript pubScript;

    // try once before we try to denominate
    if (!pwalletMain->SelectCoinsMasternode(vin, nValueIn, pubScript))
    {
        if(fDebug) LogPrintf("CDarkSendPool::GetMasterNodeVin - I'm not a capable masternode\n");
        return false;
    }

    CTxDestination address1;
    ExtractDestination(pubScript, address1);
    CBitcoinAddress address2(address1);

    CKeyID keyID;
    if (!address2.GetKeyID(keyID)) {
        LogPrintf("CDarkSendPool::GetMasterNodeVin - Address does not refer to a key");
        return false;
    }

    if (!pwalletMain->GetKey(keyID, secretKey)) {
        LogPrintf ("CDarkSendPool::GetMasterNodeVin - Private key for address is not known");
        return false;
    }

    pubkey = secretKey.GetPubKey();
    return true;
}

// when starting a masternode, this can enable to run as a hot wallet with no funds
bool CDarkSendPool::EnableHotColdMasterNode(CTxIn& vin, int64 sigTime, CService& addr)
{
    if(!fMasterNode) return false;

    isCapableMasterNode = MASTERNODE_IS_CAPABLE; 

    vinMasterNode = vin;
    masterNodeSignatureTime = sigTime;
    masterNodeSignAddr = addr;

    LogPrintf("CDarkSendPool::EnableHotColdMasterNode() - Enabled! You may shut down the cold daemon.");

    return true;
}

// 
// Bootup the masternode, look for a 1000DRK input and register on the network
// 
void CDarkSendPool::RegisterAsMasterNode(bool stop)
{
    if(!fMasterNode) return;

    //need correct adjusted time to send ping
    bool fIsInitialDownload = IsInitialBlockDownload();
    if(fIsInitialDownload) {
        isCapableMasterNode = MASTERNODE_SYNC_IN_PROCESS;
        LogPrintf("CDarkSendPool::RegisterAsMasterNode() - Sync in progress. Must wait until sync is complete to start masternode.");
        return;
    }

    std::string errorMessage;

    CKey key2;
    CPubKey pubkey2;

    if(!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, key2, pubkey2))
    {
        LogPrintf("Invalid masternodeprivkey: '%s'\n", errorMessage.c_str());
        exit(0);
    }

    if(isCapableMasterNode == MASTERNODE_INPUT_TOO_NEW || isCapableMasterNode == MASTERNODE_NOT_CAPABLE || isCapableMasterNode == MASTERNODE_SYNC_IN_PROCESS){
        isCapableMasterNode = MASTERNODE_NOT_PROCESSED;
    }

    if(isCapableMasterNode == MASTERNODE_NOT_PROCESSED) {
        if(strMasterNodeAddr.empty()) {
            if(!GetLocal(masterNodeSignAddr)) {
                LogPrintf("CDarkSendPool::RegisterAsMasterNode() - Can't detect external address. Please use the masternodeaddr configuration option.");
                isCapableMasterNode = MASTERNODE_NOT_CAPABLE;
                return;
            }
        } else {
            masterNodeSignAddr = CService(strMasterNodeAddr);
        }

        if((fTestNet && masterNodeSignAddr.GetPort() != 19999) || (!fTestNet && masterNodeSignAddr.GetPort() != 9999)) {
            LogPrintf("CDarkSendPool::RegisterAsMasterNode() - Invalid port");
            isCapableMasterNode = MASTERNODE_NOT_CAPABLE;
            exit(0);
        }

        LogPrintf("CDarkSendPool::RegisterAsMasterNode() - Checking inbound connection to '%s'\n", masterNodeSignAddr.ToString().c_str());

        if(ConnectNode((CAddress)masterNodeSignAddr, masterNodeSignAddr.ToString().c_str())){
            darkSendPool.masternodePortOpen = MASTERNODE_PORT_OPEN;
        } else {
            darkSendPool.masternodePortOpen = MASTERNODE_PORT_NOT_OPEN;
            isCapableMasterNode = MASTERNODE_NOT_CAPABLE;
            return;
        }


        if(pwalletMain->IsLocked()){
            isCapableMasterNode = MASTERNODE_NOT_CAPABLE;
            return;
        }

        isCapableMasterNode = MASTERNODE_NOT_CAPABLE;

        CKey SecretKey;
        // Choose coins to use
        if(GetMasterNodeVin(vinMasterNode, pubkeyMasterNode, SecretKey)) {

            if(GetInputAge(vinMasterNode) < MASTERNODE_MIN_CONFIRMATIONS){
                LogPrintf("CDarkSendPool::RegisterAsMasterNode() - Input must have least %d confirmations - %d confirmations\n", MASTERNODE_MIN_CONFIRMATIONS, GetInputAge(vinMasterNode));
                isCapableMasterNode = MASTERNODE_INPUT_TOO_NEW;
                return;
            }

            masterNodeSignatureTime = GetAdjustedTime();

            std::string vchPubKey(pubkeyMasterNode.begin(), pubkeyMasterNode.end());
            std::string vchPubKey2(pubkey2.begin(), pubkey2.end());
            std::string strMessage = masterNodeSignAddr.ToString() + boost::lexical_cast<std::string>(masterNodeSignatureTime) + vchPubKey + vchPubKey2;

            if(!darkSendSigner.SignMessage(strMessage, errorMessage, vchMasterNodeSignature, SecretKey)) {
                LogPrintf("CDarkSendPool::RegisterAsMasterNode() - Sign message failed");
                return;
            }

            if(!darkSendSigner.VerifyMessage(pubkeyMasterNode, vchMasterNodeSignature, strMessage, errorMessage)) {
                LogPrintf("CDarkSendPool::RegisterAsMasterNode() - Verify message failed");
                return;
            }

            LogPrintf("CDarkSendPool::RegisterAsMasterNode() - Is capable master node!\n");

            isCapableMasterNode = MASTERNODE_IS_CAPABLE; 

            pwalletMain->LockCoin(vinMasterNode.prevout);

            bool found = false;
            BOOST_FOREACH(CMasterNode& mn, darkSendMasterNodes)
                if(mn.vin == vinMasterNode)
                    found = true;

            if(!found) {                
                LogPrintf("CDarkSendPool::RegisterAsMasterNode() - Adding myself to masternode list %s - %s\n", masterNodeSignAddr.ToString().c_str(), vinMasterNode.ToString().c_str());
                CMasterNode mn(masterNodeSignAddr, vinMasterNode, pubkeyMasterNode, vchMasterNodeSignature, masterNodeSignatureTime, pubkey2);
                mn.UpdateLastSeen(masterNodeSignatureTime);
                darkSendMasterNodes.push_back(mn);
                LogPrintf("CDarkSendPool::RegisterAsMasterNode() - Masternode input = %s\n", vinMasterNode.ToString().c_str());
            }
        
            RelayDarkSendElectionEntry(vinMasterNode, masterNodeSignAddr, vchMasterNodeSignature, masterNodeSignatureTime, pubkeyMasterNode, pubkey2, -1, -1, masterNodeSignatureTime);

            return;
        }
    }

    if(isCapableMasterNode != MASTERNODE_IS_CAPABLE) return;

    masterNodeSignatureTime = GetAdjustedTime();

    std::string strMessage = masterNodeSignAddr.ToString() + boost::lexical_cast<std::string>(masterNodeSignatureTime) + boost::lexical_cast<std::string>(stop);

    if(!darkSendSigner.SignMessage(strMessage, errorMessage, vchMasterNodeSignature, key2)) {
        LogPrintf("CDarkSendPool::RegisterAsMasterNode() - Sign message failed");
        return;
    }

    if(!darkSendSigner.VerifyMessage(pubkey2, vchMasterNodeSignature, strMessage, errorMessage)) {
        LogPrintf("CDarkSendPool::RegisterAsMasterNode() - Verify message failed");
        return;
    }

    bool found = false;
    BOOST_FOREACH(CMasterNode& mn, darkSendMasterNodes) {
        //LogPrintf(" -- %s\n", mn.vin.ToString().c_str());

        if(mn.vin == vinMasterNode) {
            found = true;
            mn.UpdateLastSeen();
        }
    }
    assert(found);

    LogPrintf("CDarkSendPool::RegisterAsMasterNode() - Masternode input = %s\n", vinMasterNode.ToString().c_str());

    if (stop) isCapableMasterNode = MASTERNODE_STOPPED;

    RelayDarkSendElectionEntryPing(vinMasterNode, vchMasterNodeSignature, masterNodeSignatureTime, stop);
}

// 
// Bootup the masternode, look for a 1000DRK input and register on the network
// Takes 2 parameters to start a remote masternode
//
bool CDarkSendPool::RegisterAsMasterNodeRemoteOnly(std::string strMasterNodeAddr, std::string strMasterNodePrivKey)
{
    if(!fMasterNode) return false;

    printf("CDarkSendPool::RegisterAsMasterNodeRemoteOnly() - Address %s MasterNodePrivKey %s\n", strMasterNodeAddr.c_str(), strMasterNodePrivKey.c_str());

    std::string errorMessage;

    CKey key2;
    CPubKey pubkey2;


    if(!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, key2, pubkey2))
    {
        printf("     - Invalid masternodeprivkey: '%s'\n", errorMessage.c_str());
        return false;
    }

    CService masterNodeSignAddr = CService(strMasterNodeAddr);
    BOOST_FOREACH(CMasterNode& mn, darkSendMasterNodes){
        if(mn.addr == masterNodeSignAddr){
            printf("     - Address in use");
            return false;
        }
    }

    if((fTestNet && masterNodeSignAddr.GetPort() != 19999) || (!fTestNet && masterNodeSignAddr.GetPort() != 9999)) {
        printf("     - Invalid port");
        return false;
    }

    printf("     - Checking inbound connection to '%s'\n", masterNodeSignAddr.ToString().c_str());

    if(!ConnectNode((CAddress)masterNodeSignAddr, masterNodeSignAddr.ToString().c_str())){
        printf("     - Error connecting to port\n");
        return false;
    }

    if(pwalletMain->IsLocked()){
        printf("     - Wallet is locked\n");
        return false;
    }

    CKey SecretKey;
    CTxIn vinMasterNode;
    CPubKey pubkeyMasterNode;
    int masterNodeSignatureTime = 0;

    // Choose coins to use
    while (GetMasterNodeVin(vinMasterNode, pubkeyMasterNode, SecretKey)) {
        // don't use a vin that's registered
        bool found = false;
        BOOST_FOREACH(CMasterNode& mn, darkSendMasterNodes)
            if(mn.vin == vinMasterNode)
                continue;

        if(GetInputAge(vinMasterNode) < MASTERNODE_MIN_CONFIRMATIONS)
            continue;

        masterNodeSignatureTime = GetTimeMicros();

        std::string vchPubKey(pubkeyMasterNode.begin(), pubkeyMasterNode.end());
        std::string vchPubKey2(pubkey2.begin(), pubkey2.end());
        std::string strMessage = masterNodeSignAddr.ToString() + boost::lexical_cast<std::string>(masterNodeSignatureTime) + vchPubKey + vchPubKey2;

        if(!darkSendSigner.SignMessage(strMessage, errorMessage, vchMasterNodeSignature, SecretKey)) {
            printf("     - Sign message failed");
            return false;
        }

        if(!darkSendSigner.VerifyMessage(pubkeyMasterNode, vchMasterNodeSignature, strMessage, errorMessage)) {
            printf("     - Verify message failed");
            return false;
        }

        printf("     - Is capable master node!\n");

        pwalletMain->LockCoin(vinMasterNode.prevout);
    
        RelayDarkSendElectionEntry(vinMasterNode, masterNodeSignAddr, vchMasterNodeSignature, masterNodeSignatureTime, pubkeyMasterNode, pubkey2, -1, -1, masterNodeSignatureTime);

        return true;
    }

    printf("     - No sutable vin found\n");
    return false;
}

bool CDarkSendPool::GetBlockHash(uint256& hash, int nBlockHeight)
{
    if(unitTest){
        hash.SetHex("00000000001432b4910722303bff579d0445fa23325bdc34538bdb226718ba79");
        return true;
    }

    const CBlockIndex *BlockLastSolved = pindexBest;
    const CBlockIndex *BlockReading = pindexBest;

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0) { return false; }

    //printf(" nBlockHeight2 %"PRI64u" %"PRI64u"\n", nBlockHeight, pindexBest->nHeight+1);
   
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if(BlockReading->nHeight == nBlockHeight) {
            hash = BlockReading->GetBlockHash();
            return true;
        }

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    return false;    
}

//Get the last hash that matches the modulus given. Processed in reverse order
bool CDarkSendPool::GetLastValidBlockHash(uint256& hash, int mod, int nBlockHeight)
{
    if(unitTest){
        hash.SetHex("00000000001432b4910722303bff579d0445fa23325bdc34538bdb226718ba79");
        return true;
    }

    const CBlockIndex *BlockLastSolved = pindexBest;
    const CBlockIndex *BlockReading = pindexBest;

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0) { return false; }

    int nBlocksAgo = 0;
    if(nBlockHeight > 0) nBlocksAgo = nBlockHeight - (pindexBest->nHeight+1);
    assert(nBlocksAgo >= 0);
    
    int n = 0;
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if(BlockReading->nHeight % mod == 0) {
            if(n >= nBlocksAgo){
                hash = BlockReading->GetBlockHash();
                return true;
            }
            n++;
        }

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    return false;    
}

void CDarkSendPool::NewBlock()
{
    if(fDebug) LogPrintf("CDarkSendPool::NewBlock \n");

    if(IsInitialBlockDownload()) return;
    
    masternodePayments.ProcessBlock(pindexBest->nHeight+10);

    if(!fEnableDarksend) return;

    if(!fMasterNode){
        //denominate all non-denominated inputs every 25 minutes.
        if(pindexBest->nHeight % 10 == 0) UnlockCoins();
        ProcessMasternodeConnections();
    }
}

// Darksend transaction was completed (failed or successed)
void CDarkSendPool::CompletedTransaction(bool error, std::string lastMessageNew)
{
    if(fMasterNode) return;

    if(error){
        LogPrintf("CompletedTransaction -- error \n");
        UpdateState(POOL_STATUS_ERROR);
    } else {
        LogPrintf("CompletedTransaction -- success \n");
        UpdateState(POOL_STATUS_SUCCESS);

        myEntries.clear();

        // To avoid race conditions, we'll only let DS run once per block
        cachedLastSuccess = nBestHeight;
        splitUpInARow = 0;
    }
    lastMessage = lastMessageNew;

    completedTransaction = true;
    Check();
    UnlockCoins();
}

void CDarkSendPool::ClearLastMessage()
{
    lastMessage = "";
}

// 
// Deterministically calculate a given "score" for a masternode depending on how close it's hash is to 
// the proof of work for that block. The further away they are the better, the furthest will win the election 
// and get paid this block
// 
uint256 CMasterNode::CalculateScore(int mod, int64 nBlockHeight)
{
    if(pindexBest == NULL) return 0;

    uint256 n1 = 0;
    if(!darkSendPool.GetLastValidBlockHash(n1, mod, nBlockHeight)) return 0;

    uint256 n2 = Hash9(BEGIN(n1), END(n1));
    uint256 n3 = vin.prevout.hash > n2 ? (vin.prevout.hash - n2) : (n2 - vin.prevout.hash);

    /*
    LogPrintf(" -- MasterNode CalculateScore() n1 = %s \n", n1.ToString().c_str());
    LogPrintf(" -- MasterNode CalculateScore() n11 = %u \n", n11);
    LogPrintf(" -- MasterNode CalculateScore() n2 = %s \n", n2.ToString().c_str());
    LogPrintf(" -- MasterNode CalculateScore() vin = %s \n", vin.prevout.hash.ToString().c_str());
    LogPrintf(" -- MasterNode CalculateScore() n3 = %s \n", n3.ToString().c_str());*/
    
    return n3;
}

int CDarkSendPool::GetMasternodeByVin(CTxIn& vin)
{
    int i = 0;

    BOOST_FOREACH(CMasterNode mn, darkSendMasterNodes) {
        if (mn.vin == vin) return i;
        i++;
    }

    return -1;
}

int CDarkSendPool::GetCurrentMasterNode(int mod, int64 nBlockHeight)
{
    int i = 0;
    unsigned int score = 0;
    int winner = -1;

    // scan for winner
    BOOST_FOREACH(CMasterNode mn, darkSendMasterNodes) {
        mn.Check();
        if(!mn.IsEnabled()) {
            i++;
            continue;
        }

        // calculate the score for each masternode
        uint256 n = mn.CalculateScore(mod, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));
        
        // determine the winner
        if(n2 > score){
            score = n2;
            winner = i; 
        }
        i++;
    }

    return winner;
}

bool CMasternodePayments::CheckSignature(CMasternodePaymentWinner& winner)
{
    std::string strMessage = winner.vin.ToString().c_str() + boost::lexical_cast<std::string>(winner.nBlockHeight); 
    std::string strPubKey = fTestNet? strTestPubKey : strMainPubKey;
    CPubKey pubkey(ParseHex(strPubKey));

    std::string errorMessage = "";
    if(!darkSendSigner.VerifyMessage(pubkey, winner.vchSig, strMessage, errorMessage)){
        return false;
    }

    return true;
}

bool CMasternodePayments::Sign(CMasternodePaymentWinner& winner) 
{
    std::string strMessage = winner.vin.ToString().c_str() + boost::lexical_cast<std::string>(winner.nBlockHeight); 

    CKey key2;
    CPubKey pubkey2;
    std::string errorMessage = "";

    if(!darkSendSigner.SetKey(strMasterPrivKey, errorMessage, key2, pubkey2))
    {
        LogPrintf("Invalid masternodeprivkey: '%s'\n", errorMessage.c_str());
        exit(0);
    }

    if(!darkSendSigner.SignMessage(strMessage, errorMessage, winner.vchSig, key2)) {
        LogPrintf("CDarksendQueue():Relay - Sign message failed");
        return false;
    }

    if(!darkSendSigner.VerifyMessage(pubkey2, winner.vchSig, strMessage, errorMessage)) {
        LogPrintf("CDarksendQueue():Relay - Verify message failed");
        return false;
    }

    return true;
}

uint64 CMasternodePayments::CalculateScore(uint256 blockHash, CTxIn& vin)
{
    uint256 n1 = blockHash;
    uint256 n2 = Hash9(BEGIN(n1), END(n1));
    uint256 n3 = Hash9(BEGIN(vin.prevout.hash), END(vin.prevout.hash));
    uint256 n4 = n3 > n2 ? (n3 - n2) : (n2 - n3);

    //printf(" -- CMasternodePayments CalculateScore() n2 = %"PRI64u" \n", n2.Get64());
    //printf(" -- CMasternodePayments CalculateScore() n3 = %"PRI64u" \n", n3.Get64());
    //printf(" -- CMasternodePayments CalculateScore() n4 = %"PRI64u" \n", n4.Get64());

    return n4.Get64();
}

bool CMasternodePayments::GetBlockPayee(int nBlockHeight, CScript& payee)
{
    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning){
        if(winner.nBlockHeight == nBlockHeight) {

            CTransaction tx;
            uint256 hash;
            if(GetTransaction(winner.vin.prevout.hash, tx, hash, true)){
                BOOST_FOREACH(CTxOut out, tx.vout){
                    if(out.nValue == 1000*COIN){
                        payee = out.scriptPubKey;
                        return true;
                    }
                }
            }

            return false;
        }
    }

    return false;
}

bool CMasternodePayments::GetWinningMasternode(int nBlockHeight, CTxIn& vinOut)
{
    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning){
        if(winner.nBlockHeight == nBlockHeight) {
            vinOut = winner.vin;
            return true;
        }
    }

    return false;
}

bool CMasternodePayments::AddWinningMasternode(CMasternodePaymentWinner& winnerIn)
{
    uint256 blockHash = 0;
    if(!darkSendPool.GetBlockHash(blockHash, winnerIn.nBlockHeight-576)) {
        return false;
    }

    winnerIn.score = CalculateScore(blockHash, winnerIn.vin);

    bool foundBlock = false;
    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning){
        if(winner.nBlockHeight == winnerIn.nBlockHeight) {
            foundBlock = true;
            if(winner.score < winnerIn.score){
                winner.score = winnerIn.score;
                winner.vin = winnerIn.vin;
                winner.vchSig = winnerIn.vchSig;
                return true;
            }
        }
    }

    // if it's not in the vector
    if(!foundBlock){
         vWinning.push_back(winnerIn);
         return true;
    }

    return false;
}

void CMasternodePayments::CleanPaymentList()
{
    if(pindexBest == NULL) return;

    vector<CMasternodePaymentWinner>::iterator it;
    for(it=vWinning.begin();it<vWinning.end();it++){
        if(pindexBest->nHeight - (*it).nBlockHeight > 4){
            if(fDebug) LogPrintf("Removing old masternode payment - block %d\n", (*it).nBlockHeight);
            vWinning.erase(it);
            break;
        }
    }
}

int CMasternodePayments::LastPayment(CMasterNode& mn)
{
    if(pindexBest == NULL) return 0;

    int ret = mn.GetMasternodeInputAge();

    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning){
        if(winner.vin == mn.vin && pindexBest->nHeight - winner.nBlockHeight < ret)
            ret = pindexBest->nHeight - winner.nBlockHeight;
    }

    return ret;
}

bool CMasternodePayments::ProcessBlock(int nBlockHeight)
{
    if(strMasterPrivKey.empty()) return false;
    CMasternodePaymentWinner winner;

    uint256 blockHash = 0;
    if(!darkSendPool.GetBlockHash(blockHash, nBlockHeight-576)) return false;

    BOOST_FOREACH(CMasterNode& mn, darkSendMasterNodes) {
        if(LastPayment(mn) < darkSendMasterNodes.size()*.9) continue;

        uint64 score = CalculateScore(blockHash, mn.vin);
        if(score > winner.score){
            winner.score = score;
            winner.nBlockHeight = nBlockHeight;
            winner.vin = mn.vin;
        }
    }

    if(winner.nBlockHeight == 0) return false; //no masternodes available

    if(Sign(winner)){
        if(AddWinningMasternode(winner)){
            Relay(winner);
            return true;
        }
    }

    return false;
}

void CMasternodePayments::Relay(CMasternodePaymentWinner& winner)
{    
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        pnode->PushMessage("mnw", winner);
}

void CMasternodePayments::Sync(CNode* node)
{
    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning)
        if(winner.nBlockHeight >= pindexBest->nHeight-10 && winner.nBlockHeight <= pindexBest->nHeight + 20)
            node->PushMessage("mnw", winner);
}


bool CMasternodePayments::SetPrivKey(std::string strPrivKey)
{
    CMasternodePaymentWinner winner;

    // Test signing successful, proceed
    strMasterPrivKey = strPrivKey;

    Sign(winner);

    if(CheckSignature(winner)){
        LogPrintf("Successfully initialized as masternode payments master\n");
        return true;
    } else {
        return false;
    }
}


// 
// Passively run Darksend in the background to anonymize funds based on the given configuration.
//
// This does NOT run by default for daemons, only for QT. 
//
bool CDarkSendPool::DoAutomaticDenominating(bool fDryRun, bool ready)
{
    if(fMasterNode) return false;
    if(state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) return false;

    if(nBestHeight-cachedLastSuccess < nDarksendBlocksBetweenSuccesses) {
        LogPrintf("CDarkSendPool::DoAutomaticDenominating - Last successful darksend was too recent\n");
        return false;
    }
    if(!fEnableDarksend) {
        LogPrintf("CDarkSendPool::DoAutomaticDenominating - Darksend is disabled\n");
        return false; 
    }

    if (!fDryRun && pwalletMain->IsLocked()){
        return false;
    }

    if(darkSendPool.GetState() != POOL_STATUS_ERROR && darkSendPool.GetState() != POOL_STATUS_SUCCESS){
        if(darkSendPool.GetMyTransactionCount() > 0){
            return true;
        }
    }

    // ** find the coins we'll use
    std::vector<CTxIn> vCoins;
    int64 nValueMin = 0.01*COIN;
    int64 nValueMax = 999*COIN;
    int64 nValueIn = 0;
    int minRounds = -2; //non denominated funds are rounds of less than 0
    int maxAmount = 1000;
    bool hasFeeInput = false;

    // if we have more denominated funds (of any maturity) than the nAnonymizeDarkcoinAmount, we should use use those
    if(pwalletMain->GetDenominatedBalance(true) >= nAnonymizeDarkcoinAmount*COIN) {
        minRounds = 0;
    }
    //if we're set to less than a thousand, don't submit for than that to the pool
    if(nAnonymizeDarkcoinAmount < 1000) maxAmount = nAnonymizeDarkcoinAmount;

    int64 balanceNeedsAnonymized = pwalletMain->GetBalance() - pwalletMain->GetAnonymizedBalance();
    if(balanceNeedsAnonymized > maxAmount*COIN) balanceNeedsAnonymized= maxAmount*COIN;
    if(balanceNeedsAnonymized < COIN*2.5){
        LogPrintf("DoAutomaticDenominating : No funds detected in need of denominating \n");
        return false;
    }

    // if the balance is more the pool max, take the pool max
    if(balanceNeedsAnonymized > nValueMax) balanceNeedsAnonymized = nValueMax;

    // select coins that should be given to the pool
    if (!pwalletMain->SelectCoinsDark(nValueMin, maxAmount*COIN, vCoins, nValueIn, minRounds, nDarksendRounds, hasFeeInput))
    {
        nValueIn = 0;
        vCoins.clear();

        // look for inputs larger than the max amount, if we find anything we need to split it up
        if (pwalletMain->SelectCoinsDark(maxAmount*COIN, 9999999*COIN, vCoins, nValueIn, minRounds, nDarksendRounds, hasFeeInput))
        {
            if(!fDryRun) SplitUpMoney();
            return true;
        }

        LogPrintf("DoAutomaticDenominating : No funds detected in need of denominating (2)\n");
        return false;
    }

    // the darksend pool can only take 1.1DRK minimum
    if(nValueIn < COIN*1.1){
        //simply look for non-denominated coins
        if (pwalletMain->SelectCoinsDark(maxAmount*COIN, 9999999*COIN, vCoins, nValueIn, minRounds, nDarksendRounds, hasFeeInput))
        {
            if(!fDryRun) SplitUpMoney();
            return true;
        }

        LogPrintf("DoAutomaticDenominating : Too little to denominate (must have 1.1DRK) \n");
        return false;
    }

    //check to see if we have the fee sized inputs, it requires these
    if(!pwalletMain->HasDarksendFeeInputs()){
        if(!fDryRun) SplitUpMoney(true);
        return true;
    }

    if(fDryRun) return true;

    // initial phase, find a masternode
    if(!sessionFoundMasternode){
        int64 nTotalValue = pwalletMain->GetTotalValue(vCoins) - DARKSEND_FEE;
        if(nTotalValue > maxAmount*COIN) nTotalValue = maxAmount*COIN;
        
        double fDarkcoinSubmitted = nTotalValue / COIN;

        LogPrintf("Submiting Darksend for %f DRK\n", fDarkcoinSubmitted);

        if(pwalletMain->GetDenominatedBalance(true, true) > 0){ //get denominated unconfirmed inputs 
            LogPrintf("DoAutomaticDenominating -- Found unconfirmed denominated outputs, will wait till they confirm to continue.\n");
            return false;
        }

        // Look through the queues and see if anything matches
        BOOST_FOREACH(CDarksendQueue& dsq, vecDarksendQueue){
            CService addr;
            if(dsq.time == 0) continue;
            if(!dsq.GetAddress(addr)) continue;

            // If we don't match the denominations, we don't want to submit our inputs
            if(dsq.nDenom != GetDenominationsByAmount(nTotalValue)) {
                if(fDebug) LogPrintf(" dsq.nDenom != GetDenominationsByAmount %"PRI64d" %d \n", dsq.nDenom, GetDenominationsByAmount(nTotalValue));
                continue;
            }
            dsq.time = 0; //remove node

            // connect to masternode and submit the queue request
            if(ConnectNode((CAddress)addr, NULL, true)){
                submittedToMasternode = addr;
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    if(submittedToMasternode != pnode->addr) continue;
                
                    std::string strReason;
                    if(txCollateral == CTransaction()){
                        if(!pwalletMain->CreateCollateralTransaction(txCollateral, strReason)){
                            LogPrintf("DoAutomaticDenominating -- dsa error:%s\n", strReason.c_str());
                            return false; 
                        }
                    }
                
                    sessionAmount = nTotalValue;
                    pnode->PushMessage("dsa", nTotalValue, txCollateral);
                    LogPrintf("DoAutomaticDenominating --- connected (from queue), sending dsa for %"PRI64d" - denom %d\n", nTotalValue, GetDenominationsByAmount(nTotalValue));
                    return true;
                }
            } else {
                LogPrintf("DoAutomaticDenominating --- error connecting \n");
                return DoAutomaticDenominating();
            }
        }

        // otherwise, try one randomly
        if(sessionTries++ < 10){
            //pick a random masternode to use
            int max_value = darkSendMasterNodes.size();
            if(max_value <= 0) return false;
            int i = (rand() % max_value);

            lastTimeChanged = GetTimeMillis();
            LogPrintf("DoAutomaticDenominating -- attempt %d connection to masternode %s\n", sessionTries, darkSendMasterNodes[i].addr.ToString().c_str());
            if(ConnectNode((CAddress)darkSendMasterNodes[i].addr, NULL, true)){
                submittedToMasternode = darkSendMasterNodes[i].addr;
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    if(darkSendMasterNodes[i].addr != pnode->addr) continue;

                    std::string strReason;
                    if(txCollateral == CTransaction()){
                        if(!pwalletMain->CreateCollateralTransaction(txCollateral, strReason)){
                            LogPrintf("DoAutomaticDenominating -- dsa error:%s\n", strReason.c_str());
                            return false; 
                        }
                    }

                    sessionAmount = nTotalValue;
                    pnode->PushMessage("dsa", nTotalValue, txCollateral);
                    LogPrintf("DoAutomaticDenominating --- connected, sending dsa for %"PRI64d" - denom %d\n", nTotalValue, GetDenominationsByAmount(nTotalValue));
                    return true;
                }
            } else {
                LogPrintf("DoAutomaticDenominating --- error connecting \n");
                return DoAutomaticDenominating();
            }
        } else {
            return false;
        }
    }

    if(!ready) return true;
    if(sessionAmount == 0) return true;

    // Submit transaction to the pool if we get here, use sessionAmount so we use the same amount of money
    std::string strError = pwalletMain->PrepareDarksendDenominate(minRounds, sessionAmount);
    LogPrintf("DoAutomaticDenominating : Running darksend denominate. Return '%s'\n", strError.c_str());
    
    if(strError == "") return true;

    LogPrintf("DoAutomaticDenominating : Error running denominate, %s\n", strError.c_str());
    return false;
}

// Split up large inputs or create fee sized inputs
bool CDarkSendPool::SplitUpMoney(bool justCollateral)
{
    if((nBestHeight - lastSplitUpBlock) < 10){
        LogPrintf("SplitUpMoney - Too soon to split up again\n");
        return false;
    }

    if(splitUpInARow >= 2){
        LogPrintf("Error: Darksend SplitUpMoney was called multiple times in a row. This should not happen. Please submit a detailed explanation of the steps it took to create this error and submit to evan@darkcoin.io. \n");
        fEnableDarksend = false;
        return false;
    }

    int64 nTotalBalance = pwalletMain->GetDenominatedBalance(false);
    if(justCollateral && nTotalBalance > 1*COIN) nTotalBalance = 1*COIN;
    int64 nTotalOut = 0;
    lastSplitUpBlock = nBestHeight;

    LogPrintf("DoAutomaticDenominating: Split up large input (justCollateral %d):\n", justCollateral);
    LogPrintf(" -- nTotalBalance %"PRI64d"\n", nTotalBalance);
    LogPrintf(" -- non-denom %"PRI64d" \n", pwalletMain->GetDenominatedBalance(false));

    // make our change address
    CReserveKey reservekey(pwalletMain);

    CScript scriptChange;
    CPubKey vchPubKey;
    assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptChange.SetDestination(vchPubKey.GetID());

    CWalletTx wtx;
    int64 nFeeRet = 0;
    std::string strFail = "";
    vector< pair<CScript, int64> > vecSend;

    int64 a = 1*COIN;

    // ****** Add fees ************ /
    vecSend.push_back(make_pair(scriptChange, (DARKSEND_COLLATERAL*5)+DARKSEND_FEE));
    nTotalOut += (DARKSEND_COLLATERAL*5)+DARKSEND_FEE; 

    for(int d = 0; d <= std::min(4+nDarksendRounds,20); d++){
        vecSend.push_back(make_pair(scriptChange, DARKSEND_FEE));
        nTotalOut += DARKSEND_FEE;
    }


    // ****** Add outputs in bases of two from 1 darkcoin *** /
    if(!justCollateral){
        bool continuing = true;

        while(continuing){
            if(nTotalOut + a < nTotalBalance-DARKSEND_FEE){
                LogPrintf("SplitUpMoney: nTotalOut %"PRI64d", added %"PRI64d"\n", nTotalOut, a);

                vecSend.push_back(make_pair(scriptChange, a));
                nTotalOut += a;
            } else {
                continuing = false;
            }

            a = a * 2;
        }
    }

    LogPrintf(" -- nTotalOut %"PRI64d" \n", nTotalOut);

    if((justCollateral && nTotalOut <= 0.1*COIN) || vecSend.size() < 3) {
        LogPrintf("SplitUpMoney: Not enough outputs to make a transaction\n");
        return false;
    }
    if((!justCollateral && nTotalOut <= 1.1*COIN) || vecSend.size() < 3){
        LogPrintf("SplitUpMoney: Not enough outputs to make a transaction\n");
        return false;
    }

    CCoinControl *coinControl=NULL;
    bool success = pwalletMain->CreateTransaction(vecSend, wtx, reservekey, nFeeRet, strFail, coinControl, ONLY_NONDENOMINATED);
    if(!success){
        LogPrintf("SplitUpMoney: Error - %s\n", strFail.c_str());
        return false;
    }

    pwalletMain->CommitTransaction(wtx, reservekey);

    LogPrintf("SplitUpMoney Success: tx %s\n", wtx.GetHash().GetHex().c_str());

    splitUpInARow++;
    return true;
}

int CDarkSendPool::GetMasternodeByRank(int findRank)
{
    int i = 0;
 
    std::vector<pair<unsigned int, int> > vecMasternodeScores;

    i = 0;
    BOOST_FOREACH(CMasterNode mn, darkSendMasterNodes) {
        mn.Check();
        if(!mn.IsEnabled()) {
            i++;
            continue;
        }

        uint256 n = mn.CalculateScore();
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, i));
        i++;
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnly2());
    
    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, int)& s, vecMasternodeScores){
        rank++;
        if(rank == findRank) return s.second;
    }

    return -1;
}

int CDarkSendPool::GetMasternodeRank(CTxIn& vin, int mod)
{
    std::vector<pair<unsigned int, CTxIn> > vecMasternodeScores;

    BOOST_FOREACH(CMasterNode mn, darkSendMasterNodes) {
        mn.Check();
        if(!mn.IsEnabled()) {
            continue;
        }

        uint256 n = mn.CalculateScore(mod);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, mn.vin));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnly());
    
    unsigned int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, CTxIn)& s, vecMasternodeScores){
        rank++;
        if(s.second == vin) return rank;
    }

    return -1;
}

// Recursively determine the rounds of a given input (How deep is the darksend chain for a given input)
int CDarkSendPool::GetInputDarksendRounds(CTxIn in, int rounds)
{
    if(rounds >= 9) return rounds;

    std::string padding = "";
    padding.insert(0, ((rounds+1)*5)+3, ' ');

    CWalletTx tx;
    if(pwalletMain->GetTransaction(in.prevout.hash,tx)){
        // bounds check
        if(in.prevout.n >= tx.vout.size()) return -4;

        if(tx.vout[in.prevout.n].nValue == DARKSEND_FEE) return -3;

        if(rounds == 0){ //make sure the final output is non-denominate
            bool found = false;
            BOOST_FOREACH(int64 d, darkSendDenominations)
                if(tx.vout[in.prevout.n].nValue == d) found = true;

            if(!found) {
                //LogPrintf(" - NOT DENOM\n");
                return -2;
            }
        }
        bool found = false;

        BOOST_FOREACH(CTxOut out, tx.vout){
            BOOST_FOREACH(int64 d, darkSendDenominations)
                if(out.nValue == d)
                    found = true;
        }
        
        if(!found) {
            //LogPrintf(" - NOT FOUND\n");
            return rounds;
        }

        // find my vin and look that up
        BOOST_FOREACH(CTxIn in2, tx.vin) {
            if(pwalletMain->IsMine(in2)){
                //LogPrintf("rounds :: %s %s %d NEXT\n", padding.c_str(), in.ToString().c_str(), rounds);  
                int n = GetInputDarksendRounds(in2, rounds+1);
                if(n != -3) return n;
            }
        }
    } else {
        //LogPrintf("rounds :: %s %s %d NOTFOUND\n", padding.c_str(), in.ToString().c_str(), rounds);
    }

    return rounds-1;
}

void CMasterNode::Check()
{
    //once spent, stop doing the checks
    if(enabled==3) return;

    if(!UpdatedWithin(MASTERNODE_REMOVAL_SECONDS)){
        enabled = 4;
        return;
    }

    if(!UpdatedWithin(MASTERNODE_EXPIRATION_SECONDS)){
        enabled = 2;
        return;
    }

    if(!unitTest){
        CValidationState state;
        CTransaction tx = CTransaction();
        CTxOut vout = CTxOut(999.99*COIN, darkSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);

        if(!tx.AcceptableInputs(state, true)) {
            enabled = 3;
            return; 
        }
    }

    enabled = 1; // OK
}


bool CDarkSendPool::IsCompatibleWithEntries(std::vector<CTxOut> vout)
{
    BOOST_FOREACH(const CDarkSendEntry v, entries) {
        LogPrintf(" IsCompatibleWithEntries %d %d\n", GetDenominations(vout), GetDenominations(v.vout));

        BOOST_FOREACH(CTxOut o1, vout)
            LogPrintf(" vout 1 - %s\n", o1.ToString().c_str());

        BOOST_FOREACH(CTxOut o2, v.vout)
            LogPrintf(" vout 2 - %s\n", o2.ToString().c_str());

        if(GetDenominations(vout) != GetDenominations(v.vout)) return false;
    }

    return true;
}

bool CDarkSendPool::IsCompatibleWithSession(int64 nAmount, CTransaction txCollateral, std::string& strReason)
{
    LogPrintf("CDarkSendPool::IsCompatibleWithSession - sessionAmount %"PRI64d" sessionUsers %d\n", sessionAmount, sessionUsers);

    if (!unitTest && !IsCollateralValid(txCollateral)){
        if(fDebug) LogPrintf ("CDarkSendPool::IsCompatibleWithSession - collateral not valid!\n");
        strReason = "collateral not valid";
        return false;
    }

    if(sessionUsers < 0) sessionUsers = 0;
    
    if(sessionAmount == 0) {
        sessionAmount = nAmount;
        sessionUsers++;
        lastTimeChanged = GetTimeMillis();
        entries.clear();

        if(!unitTest){
            //broadcast that I'm accepting entries, only if it's the first entry though
            CDarksendQueue dsq;
            dsq.nDenom = GetDenominationsByAmount(nAmount);
            dsq.vin = vinMasterNode;
            dsq.time = GetTime();
            dsq.Sign();
            dsq.Relay();
        }

        UpdateState(POOL_STATUS_QUEUE);
        vecSessionCollateral.push_back(txCollateral);
        return true;
    }

    if((state != POOL_STATUS_ACCEPTING_ENTRIES && state != POOL_STATUS_QUEUE) || sessionUsers >= GetMaxPoolTransactions()){
        if((state != POOL_STATUS_ACCEPTING_ENTRIES && state != POOL_STATUS_QUEUE)) strReason = "incompatible mode";
        if(sessionUsers >= GetMaxPoolTransactions()) strReason = "masternode queue is full";
        LogPrintf("CDarkSendPool::IsCompatibleWithSession - incompatible mode, return false %d %d\n", state != POOL_STATUS_ACCEPTING_ENTRIES, sessionUsers >= GetMaxPoolTransactions());
        return false;
    }

    if(GetDenominationsByAmount(nAmount) != GetDenominationsByAmount(sessionAmount)) {
        strReason = "no matching denominations found for mixing";
        return false;
    }

    LogPrintf("CDarkSendPool::IsCompatibleWithSession - compatible\n");

    sessionUsers++;
    lastTimeChanged = GetTimeMillis();
    vecSessionCollateral.push_back(txCollateral);

    return true;
}

// return a bitshifted integer representing the denominations in this list
int CDarkSendPool::GetDenominations(const std::vector<CTxOut>& vout){
    std::vector<pair<int64, int> > denomUsed;

    // make a list of denominations, with zero uses
    BOOST_FOREACH(int64 d, darkSendDenominations)
        denomUsed.push_back(make_pair(d, 0));

    // look for denominations and update uses to 1
    BOOST_FOREACH(CTxOut out, vout)
        BOOST_FOREACH (PAIRTYPE(int64, int)& s, denomUsed)
            if (out.nValue == s.first)
                s.second = 1;

    int denom = 0;
    int c = 0;
    // if the denomination is used, shift the bit on.
    // then move to the next
    BOOST_FOREACH (PAIRTYPE(int64, int)& s, denomUsed)
        denom |= s.second << c++;

    // Function returns as follows:
    //
    // bit 0 - 500DRK+1 ( bit on if present )
    // bit 1 - 100DRK+1 
    // bit 2 - 10DRK+1 
    // bit 3 - 1DRK+1 
    //
    // 

    return denom;
}

// calculate the outputs from a given amount of darkcoin and 
// return the bitshifted integer to represent them
int CDarkSendPool::GetDenominationsByAmount(int64 nAmount){
    CScript e = CScript();
    int64 nValueLeft = nAmount;

    std::vector<CTxOut> vout1;
    BOOST_FOREACH(int64 v, darkSendDenominations){
        int nOutputs = 0;
        while(nValueLeft - v >= 0 && nOutputs <= 10) {
            CTxOut o(v, e);
            vout1.push_back(o);
            nValueLeft -= v;
            nOutputs++;
        }
    }

    return GetDenominations(vout1);
}

bool CDarkSendSigner::IsVinAssociatedWithPubkey(CTxIn& vin, CPubKey& pubkey){
    CScript payee2;
    payee2.SetDestination(pubkey.GetID());

    CTransaction txVin;
    uint256 hash;
    if(GetTransaction(vin.prevout.hash, txVin, hash, true)){
        BOOST_FOREACH(CTxOut out, txVin.vout){
            if(out.nValue == 1000*COIN){
                if(out.scriptPubKey == payee2) return true;
            }
        }
    }

    return false;
}

bool CDarkSendSigner::SetKey(std::string strSecret, std::string& errorMessage, CKey& key, CPubKey& pubkey){
    CBitcoinSecret vchSecret;
    bool fGood = vchSecret.SetString(strSecret);

    if (!fGood) {
        errorMessage = "Invalid private key";
        return false;
    }     

    key = vchSecret.GetKey();
    pubkey = key.GetPubKey();

    return true;
}

bool CDarkSendSigner::SignMessage(std::string strMessage, std::string& errorMessage, vector<unsigned char>& vchSig, CKey key)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    if (!key.SignCompact(ss.GetHash(), vchSig)) {
        errorMessage = "Sign failed";
        return false;
    }

    return true;
}

bool CDarkSendSigner::VerifyMessage(CPubKey pubkey, vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CPubKey pubkey2;
    if (!pubkey2.RecoverCompact(ss.GetHash(), vchSig)) {
        errorMessage = "Error recovering pubkey";
        return false;
    }

    return (pubkey2.GetID() == pubkey.GetID());
}

bool CDarksendQueue::Sign()
{
    if(!fMasterNode) return false;

    std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) + boost::lexical_cast<std::string>(time) + boost::lexical_cast<std::string>(ready); 

    CKey key2;
    CPubKey pubkey2;
    std::string errorMessage = "";

    if(!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, key2, pubkey2))
    {
        LogPrintf("Invalid masternodeprivkey: '%s'\n", errorMessage.c_str());
        exit(0);
    }

    if(!darkSendSigner.SignMessage(strMessage, errorMessage, vchSig, key2)) {
        LogPrintf("CDarksendQueue():Relay - Sign message failed");
        return false;
    }

    if(!darkSendSigner.VerifyMessage(pubkey2, vchSig, strMessage, errorMessage)) {
        LogPrintf("CDarksendQueue():Relay - Verify message failed");
        return false;
    }

    return true;
}

bool CDarksendQueue::Relay()
{

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes){
        pnode->PushMessage("dsq", (*this));
    }

    return true;
}

bool CDarksendQueue::CheckSignature()
{
    BOOST_FOREACH(CMasterNode& mn, darkSendMasterNodes) {

        if(mn.vin == vin) {
            std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) + boost::lexical_cast<std::string>(time) + boost::lexical_cast<std::string>(ready); 

            std::string errorMessage = "";
            if(!darkSendSigner.VerifyMessage(mn.pubkey2, vchSig, strMessage, errorMessage)){
                return error("Got bad masternode address signature %s \n", vin.ToString().c_str());
            }

            return true;
        }
    }

    return false;
}



void ThreadCheckDarkSendPool()
{
    // Make this thread recognisable as the wallet flushing thread
    RenameThread("bitcoin-darksend");

    unsigned int c = 0;
    while (true)
    {
        MilliSleep(1000);
        //LogPrintf("ThreadCheckDarkSendPool::check timeout\n");
        darkSendPool.CheckTimeout();
        
        if(c % 60 == 0){
            vector<CMasterNode>::iterator it = darkSendMasterNodes.begin();
            while(it != darkSendMasterNodes.end()){
                (*it).Check();
                if((*it).enabled == 4 || (*it).enabled == 3){
                    LogPrintf("Removing inactive masternode %s\n", (*it).addr.ToString().c_str());
                    it = darkSendMasterNodes.erase(it);
                } else {
                    ++it;
                }
            }

            masternodePayments.CleanPaymentList();
        }


        //try to sync the masternode list and payment list every 20 seconds
        if(c % 5 == 0 && RequestedMasterNodeList <= 2){
            bool fIsInitialDownload = IsInitialBlockDownload();
            if(!fIsInitialDownload) {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    if (pnode->nVersion >= darkSendPool.MIN_PEER_PROTO_VERSION) {

                        //keep track of who we've asked for the list
                        if(pnode->HasFulfilledRequest("mnsync")) continue;
                        pnode->FulfilledRequest("mnsync");

                        LogPrintf("Successfully synced, asking for Masternode list and payment list\n");
        
                        pnode->PushMessage("dseg", CTxIn()); //request full mn list
                        pnode->PushMessage("mnget"); //sync payees
                        RequestedMasterNodeList++;
                    }
                }
            }
        }


        if(c == MASTERNODE_PING_SECONDS){
            darkSendPool.RegisterAsMasterNode(false);
            c = 0;
        }

        //auto denom every 2.5 minutes
        if(c % 60 == 0){
            darkSendPool.DoAutomaticDenominating();
        }
        c++;
    }
}
