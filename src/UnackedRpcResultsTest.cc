/* Copyright (c) 2014 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "TestUtil.h"
#include "UnackedRpcResults.h"
#include "MasterService.h"
#include "MockCluster.h"

namespace RAMCloud {

class UnackedRpcResultsTest : public ::testing::Test {
  public:
    TestLog::Enable logEnabler;
    Context context;
    ServerList serverList;
    MockCluster cluster;
    ServerConfig backup1Config;
    ServerId backup1Id;

    ServerConfig masterConfig;
    MasterService* service;
    Server* masterServer;

    UnackedRpcResults results;

    UnackedRpcResultsTest()
        : logEnabler()
        , context()
        , serverList(&context)
        , cluster(&context)
        , backup1Config(ServerConfig::forTesting())
        , backup1Id()
        , masterConfig(ServerConfig::forTesting())
        , service()
        , masterServer()
        , results(&context)
    {
        /////////////////////////////////
        // MasterService Initialization
        /////////////////////////////////
        uint32_t segmentSize = 256 * 1024;
        Logger::get().setLogLevels(RAMCloud::SILENT_LOG_LEVEL);

        backup1Config.localLocator = "mock:host=backup1";
        backup1Config.services = {WireFormat::BACKUP_SERVICE,
                WireFormat::MEMBERSHIP_SERVICE};
        backup1Config.segmentSize = segmentSize;
        backup1Config.backup.numSegmentFrames = 30;
        Server* server = cluster.addServer(backup1Config);
        server->backup->testingSkipCallerIdCheck = true;
        backup1Id = server->serverId;

        masterConfig = ServerConfig::forTesting();
        masterConfig.segmentSize = segmentSize;
        masterConfig.maxObjectDataSize = segmentSize / 4;
        masterConfig.localLocator = "mock:host=master";
        masterConfig.services = {WireFormat::MASTER_SERVICE,
                WireFormat::MEMBERSHIP_SERVICE};
        masterConfig.master.logBytes = segmentSize * 30;
        masterConfig.master.numReplicas = 1;
        masterServer = cluster.addServer(masterConfig);
        service = masterServer->master.get();
        service->objectManager.log.sync();
        context.masterService = service;
        ////////////////////////////////////////
        // End of MasterService Initialization
        ////////////////////////////////////////

        void* result;
        results.checkDuplicate(1, 10, 5, 1, &result);
        results.recordCompletion(1, 10, reinterpret_cast<void*>(1010));
    }

    DISALLOW_COPY_AND_ASSIGN(UnackedRpcResultsTest);
};

TEST_F(UnackedRpcResultsTest, checkDuplicate) {
    void* result;

    //1. New client
    EXPECT_FALSE(results.checkDuplicate(2, 1, 0, 1, &result));

    //2. Stale Rpc.
    EXPECT_THROW(results.checkDuplicate(1, 4, 3, 1, &result),
                 StaleRpcException);

    //3. Fast-path new RPC (rpcId > maxRpcId == true).
    EXPECT_EQ(10UL, results.clients[1]->maxRpcId);
    EXPECT_FALSE(results.checkDuplicate(1, 11, 6, 1, &result));
    EXPECT_EQ(0UL, (uint64_t)result);
    EXPECT_EQ(11UL, results.clients[1]->maxRpcId);
    EXPECT_EQ(6UL, results.clients[1]->maxAckId);

    EXPECT_TRUE(results.checkDuplicate(1, 11, 6, 1, &result));
    EXPECT_EQ(0UL, (uint64_t)result);

    //4. Duplicate RPC.
    EXPECT_TRUE(results.checkDuplicate(1, 10, 6, 1, &result));
    EXPECT_EQ(1010UL, (uint64_t)result);
    EXPECT_EQ(6UL, results.clients[1]->maxAckId);

    //5. Inside the window and new RPC.
    EXPECT_FALSE(results.checkDuplicate(1, 9, 7, 1, &result));
    EXPECT_EQ(0UL, (uint64_t)result);
    EXPECT_EQ(7UL, results.clients[1]->maxAckId);

    EXPECT_TRUE(results.checkDuplicate(1, 9, 7, 1, &result));
    EXPECT_EQ(0UL, (uint64_t)result);
}

TEST_F(UnackedRpcResultsTest, shouldRecover) {
    //Basic Function
    EXPECT_TRUE(results.shouldRecover(1, 10, 5));
    EXPECT_TRUE(results.shouldRecover(1, 11, 5));
    EXPECT_FALSE(results.shouldRecover(1, 5, 4));

    //Auto client insertion
    EXPECT_TRUE(results.shouldRecover(2, 4, 2)); //ClientId = 2 inserted.
    std::unordered_map<uint64_t, UnackedRpcResults::Client*>::iterator it;
    it = results.clients.find(2);
    EXPECT_NE(it, results.clients.end());

    //Ack update
    UnackedRpcResults::Client* client = it->second;
    EXPECT_EQ(2UL, client->maxAckId);
}

TEST_F(UnackedRpcResultsTest, recordCompletion) {
    void* result;
    EXPECT_FALSE(results.checkDuplicate(1, 11, 5, 1, &result));
    EXPECT_EQ(0UL, (uint64_t)result);
    results.recordCompletion(1, 11, reinterpret_cast<void*>(1011));
    EXPECT_TRUE(results.checkDuplicate(1, 11, 5, 1, &result));
    EXPECT_EQ(1011UL, (uint64_t)result);

    //Reusing spaces for acked rpcs.
    results.checkDuplicate(1, 12, 5, 1, &result);
    results.recordCompletion(1, 12, reinterpret_cast<void*>(1012));
    results.checkDuplicate(1, 13, 5, 1, &result);
    results.recordCompletion(1, 13, reinterpret_cast<void*>(1013));
    results.checkDuplicate(1, 14, 10, 1, &result);
    results.recordCompletion(1, 14, reinterpret_cast<void*>(1014));
    results.checkDuplicate(1, 15, 11, 1, &result);   //Ack up to rpcId = 11.
    results.recordCompletion(1, 15, reinterpret_cast<void*>(1015));
    results.checkDuplicate(1, 16, 5, 1, &result);
    results.recordCompletion(1, 16, reinterpret_cast<void*>(1016));

    //Ignore if acked and ignoreIfAck flag is set.
    results.recordCompletion(1, 4, reinterpret_cast<void*>(1012), true);
    results.recordCompletion(10, 1, reinterpret_cast<void*>(1012), true);

    EXPECT_EQ(16UL, results.clients[1]->maxRpcId);
    //TODO(seojin): modify test after fixing RAM-716.
    EXPECT_EQ(50, results.clients[1]->len);

    //Resized Client keeps the original data.
    results.checkDuplicate(1, 17, 5, 1, &result);

    //TODO(seojin): modify test after fixing RAM-716.
    EXPECT_EQ(50, results.clients[1]->len);
    for (int i = 12; i <= 16; ++i) {
        EXPECT_TRUE(results.checkDuplicate(1, i, 5, 1, &result));
        EXPECT_EQ((uint64_t)(i + 1000), (uint64_t)result);
    }
    EXPECT_TRUE(results.checkDuplicate(1, 17, 5, 1, &result));
    EXPECT_EQ(0UL, (uint64_t)result);

    results.recordCompletion(1, 17, reinterpret_cast<void*>(1017));
    EXPECT_TRUE(results.checkDuplicate(1, 17, 5, 1, &result));
    EXPECT_EQ(1017UL, (uint64_t)result);
}

TEST_F(UnackedRpcResultsTest, recoverRecord) {
    TestLog::Enable _("recoverRecord");
    void* result;
    uint64_t leaseId = 10;

    UnackedRpcResults::ClientMap::iterator it = results.clients.find(leaseId);
    EXPECT_TRUE(it == results.clients.end());

    // New Record w/ rpcId or ackId updates.

    results.recoverRecord(leaseId, 20, 10, &result);

    it = results.clients.find(leaseId);
    EXPECT_FALSE(it == results.clients.end());
    EXPECT_EQ(10U, it->second->maxAckId);
    EXPECT_EQ(20U, it->second->maxRpcId);
    EXPECT_TRUE(it->second->hasRecord(20));

    // New Record w/o rpcId or ackId updates.
    EXPECT_FALSE(it->second->hasRecord(15));

    results.recoverRecord(leaseId, 15, 5, &result);

    it = results.clients.find(leaseId);
    EXPECT_FALSE(it == results.clients.end());
    EXPECT_EQ(10U, it->second->maxAckId);
    EXPECT_EQ(20U, it->second->maxRpcId);
    EXPECT_TRUE(it->second->hasRecord(15));

    // Unnecessary record.
    EXPECT_FALSE(it->second->hasRecord(5));

    results.recoverRecord(leaseId, 5, 1, &result);

    it = results.clients.find(leaseId);
    EXPECT_FALSE(it == results.clients.end());
    EXPECT_FALSE(it->second->hasRecord(5));

    // Duplicate record.
//    TestLog::reset();
    results.recoverRecord(leaseId, 15, 5, &result);
    EXPECT_EQ("recoverRecord: Duplicate RpcResult found during recovery. "
              "<clientID, rpcID, ackId> = <10, 15, 5>",
            TestLog::get());
}

TEST_F(UnackedRpcResultsTest, isRpcAcked) {
    //1. Cleaned up client.
    EXPECT_TRUE(results.isRpcAcked(2, 1));

    //2. Existing client.
    EXPECT_TRUE(results.isRpcAcked(1, 5));
    EXPECT_FALSE(results.isRpcAcked(1, 6));
}

TEST_F(UnackedRpcResultsTest, cleanByTimeout) {
    void* result;
    results.cleanByTimeout();
    EXPECT_EQ(1U, results.clients.size());
    results.checkDuplicate(2, 10, 5, 1, &result);
    results.checkDuplicate(3, 10, 5, 1, &result);

    results.cleanByTimeout();
    EXPECT_EQ(3U, results.clients.size());

    results.checkDuplicate(2, 11, 10, 2, &result);

    service->updateClusterTime(1);

    results.cleanByTimeout();
    EXPECT_EQ(2U, results.clients.size());

    //Complete in progress rpcs and try cleanup again.
    results.recordCompletion(3, 10, &result);
    results.cleanByTimeout();
    EXPECT_EQ(1U, results.clients.size());

    EXPECT_EQ(1U, service->clusterTime.load());

    //TODO(seojin): test with mock coordinator which returns
    //              valid lease and we just update leaseExpiration.
}

//TODO(seojin): tests for API functions.
TEST_F(UnackedRpcResultsTest, hasRecord) {
    UnackedRpcResults::Client *client = results.clients[1];
    EXPECT_TRUE(client->hasRecord(10));
}

TEST_F(UnackedRpcResultsTest, result) {
    UnackedRpcResults::Client *client = results.clients[1];
    EXPECT_EQ(1010UL, (uint64_t)client->result(10));
}

TEST_F(UnackedRpcResultsTest, recordNewRpc) {
    UnackedRpcResults::Client *client = results.clients[1];
    client->recordNewRpc(11);
    EXPECT_TRUE(client->hasRecord(11));

    //Invoke resizing and test if all of the records are kept.
    int startRpcId = 12;
    int originalLen = client->len;
    for (int i = startRpcId; i < startRpcId + 2 * originalLen; ++i) {
        client->recordNewRpc(i);
    }
    EXPECT_NE(originalLen, client->len);
    for (int i = startRpcId; i < startRpcId + 2 * originalLen; ++i) {
        EXPECT_TRUE(client->hasRecord(i));
        EXPECT_EQ(0UL, (uint64_t)client->result(i));
    }
    EXPECT_EQ(1010UL, (uint64_t)client->result(10));
}

TEST_F(UnackedRpcResultsTest, updateResult) {
    UnackedRpcResults::Client *client = results.clients[1];
    EXPECT_EQ(1010UL, (uint64_t)client->result(10));
    client->updateResult(10, reinterpret_cast<void*>(1099));
    EXPECT_EQ(1099UL, (uint64_t)client->result(10));

    client->recordNewRpc(11);
    EXPECT_EQ(0UL, (uint64_t)client->result(11));
    client->updateResult(11, reinterpret_cast<void*>(1011));
    EXPECT_EQ(1011UL, (uint64_t)client->result(11));
}

TEST_F(UnackedRpcResultsTest, unackedRpcHandle) {
    void* result;
    { // 1. Normal use case test. No interruption between.
        UnackedRpcHandle urh(&results, 1, 11, 5, 1);
        EXPECT_FALSE(urh.isDuplicate());
        EXPECT_EQ(0UL, urh.resultLoc());
        urh.recordCompletion(1011);
    }
    EXPECT_TRUE(results.checkDuplicate(1, 11, 5, 1, &result));
    EXPECT_EQ(1011UL, (uint64_t)result);

    { //2. Duplicate RPC exists.
        UnackedRpcHandle urh(&results, 1, 11, 5, 1);
        EXPECT_TRUE(urh.isDuplicate());
        EXPECT_EQ(1011UL, urh.resultLoc());
    }
    { //3. In-progress RPC exists.
        {
            UnackedRpcHandle urh(&results, 1, 12, 5, 1);
            EXPECT_FALSE(urh.isDuplicate());
            EXPECT_EQ(0UL, urh.resultLoc());

            UnackedRpcHandle urh2(&results, 1, 12, 5, 1);
            EXPECT_TRUE(urh2.isDuplicate());
            EXPECT_TRUE(urh2.isInProgress());

            urh.recordCompletion(1012);
        }
        UnackedRpcHandle urh3(&results, 1, 12, 5, 1);
        EXPECT_TRUE(urh3.isDuplicate());
        EXPECT_FALSE(urh3.isInProgress());
        EXPECT_EQ(1012UL, urh3.resultLoc());
    }
    { //4. Reset of record by out-of-scope.
        {
            UnackedRpcHandle urh(&results, 1, 13, 5, 1);
            EXPECT_FALSE(urh.isDuplicate());
            EXPECT_EQ(0UL, urh.resultLoc());
        }
        UnackedRpcHandle urh2(&results, 1, 13, 5, 1);
        EXPECT_FALSE(urh2.isDuplicate());
    }
    { //5. After recordCompletion, don't reset of record by out-of-scope.
        {
            UnackedRpcHandle urh(&results, 1, 14, 5, 1);
            EXPECT_FALSE(urh.isDuplicate());
            EXPECT_EQ(0UL, urh.resultLoc());
            urh.recordCompletion(1014);
        }
        UnackedRpcHandle urh2(&results, 1, 14, 5, 1);
        EXPECT_TRUE(urh2.isDuplicate());
        EXPECT_EQ(1014UL, urh2.resultLoc());
    }
}

}  // namespace RAMCloud
