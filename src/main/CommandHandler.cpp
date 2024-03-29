// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/CommandHandler.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "lib/http/server.hpp"
#include "lib/json/json.h"
#include "lib/util/format.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/Maintainer.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManager.h"
#include "util/Logging.h"
#include "util/StatusManager.h"
#include "util/make_unique.h"

#include "medida/reporting/json_reporter.h"
#include "util/basen.h"
#include "xdrpp/marshal.h"
#include "xdrpp/printer.h"

#include "ExternalQueue.h"

#include "test/TestAccount.h"
#include "test/TxTests.h"
#include <regex>

using namespace stellar::txtest;

using std::placeholders::_1;
using std::placeholders::_2;

namespace stellar
{
using xdr::operator<;

CommandHandler::CommandHandler(Application& app) : mApp(app)
{
    if (mApp.getConfig().HTTP_PORT)
    {
        std::string ipStr;
        if (mApp.getConfig().PUBLIC_HTTP_PORT)
        {
            ipStr = "0.0.0.0";
        }
        else
        {
            ipStr = "127.0.0.1";
        }
        LOG(INFO) << "Listening on " << ipStr << ":"
                  << mApp.getConfig().HTTP_PORT << " for HTTP requests";

        int httpMaxClient = mApp.getConfig().HTTP_MAX_CLIENT;

        mServer = stellar::make_unique<http::server::server>(
            app.getClock().getIOService(), ipStr, mApp.getConfig().HTTP_PORT,
            httpMaxClient);
    }
    else
    {
        mServer = stellar::make_unique<http::server::server>(
            app.getClock().getIOService());
    }

    mServer->add404(std::bind(&CommandHandler::fileNotFound, this, _1, _2));

    addRoute("bans", &CommandHandler::bans);
    addRoute("catchup", &CommandHandler::catchup);
    addRoute("checkdb", &CommandHandler::checkdb);
    addRoute("connect", &CommandHandler::connect);
    addRoute("dropcursor", &CommandHandler::dropcursor);
    addRoute("droppeer", &CommandHandler::dropPeer);
    addRoute("generateload", &CommandHandler::generateLoad);
    addRoute("getcursor", &CommandHandler::getcursor);
    addRoute("info", &CommandHandler::info);
    addRoute("ll", &CommandHandler::ll);
    addRoute("logrotate", &CommandHandler::logRotate);
    addRoute("maintenance", &CommandHandler::maintenance);
    addRoute("manualclose", &CommandHandler::manualClose);
    addRoute("metrics", &CommandHandler::metrics);
    addRoute("peers", &CommandHandler::peers);
    addRoute("quorum", &CommandHandler::quorum);
    addRoute("setcursor", &CommandHandler::setcursor);
    addRoute("scp", &CommandHandler::scpInfo);
    addRoute("testacc", &CommandHandler::testAcc);
    addRoute("testtx", &CommandHandler::testTx);
    addRoute("tx", &CommandHandler::tx);
    addRoute("upgrades", &CommandHandler::upgrades);
    addRoute("unban", &CommandHandler::unban);
}

void
CommandHandler::addRoute(std::string const& name, HandlerRoute route)
{
    mServer->addRoute(
        name, std::bind(&CommandHandler::safeRouter, this, route, _1, _2));
}

void
CommandHandler::safeRouter(CommandHandler::HandlerRoute route,
                           std::string const& params, std::string& retStr)
{
    try
    {
        route(this, params, retStr);
    }
    catch (std::exception& e)
    {
        retStr =
            (fmt::MemoryWriter() << "{\"exception\": \"" << e.what() << "\"}")
                .str();
    }
    catch (...)
    {
        retStr = "{\"exception\": \"generic\"}";
    }
}

void
CommandHandler::manualCmd(std::string const& cmd)
{
    http::server::reply reply;
    http::server::request request;
    request.uri = cmd;
    mServer->handle_request(request, reply);
    LOG(INFO) << cmd << " -> " << reply.content;
}

void
CommandHandler::testAcc(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);
    Json::Value root;
    auto accName = retMap.find("name");
    if (accName == retMap.end())
    {
        root["status"] = "error";
        root["detail"] = "Bad HTTP GET: try something like: testacc?name=bob";
    }
    else
    {
        SecretKey key;
        if (accName->second == "root")
        {
            key = getRoot(mApp.getNetworkID());
        }
        else
        {
            key = getAccount(accName->second.c_str());
        }
        auto acc = loadAccount(key.getPublicKey(), mApp, false);
        if (acc)
        {
            root["name"] = accName->second;
            root["id"] = KeyUtils::toStrKey(acc->getID());
            root["balance"] = (Json::Int64)acc->getBalance();
            root["seqnum"] = (Json::UInt64)acc->getSeqNum();
        }
    }
    retStr = root.toStyledString();
}

void
CommandHandler::testTx(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    auto to = retMap.find("to");
    auto from = retMap.find("from");
    auto amount = retMap.find("amount");
    auto create = retMap.find("create");

    Json::Value root;

    if (to != retMap.end() && from != retMap.end() && amount != retMap.end())
    {
        Hash const& networkID = mApp.getNetworkID();

        auto toAccount =
            to->second == "root"
                ? TestAccount{mApp, getRoot(networkID)}
                : TestAccount{mApp, getAccount(to->second.c_str())};
        auto fromAccount =
            from->second == "root"
                ? TestAccount{mApp, getRoot(networkID)}
                : TestAccount{mApp, getAccount(from->second.c_str())};

        uint64_t paymentAmount = 0;
        std::istringstream iss(amount->second);
        iss >> paymentAmount;

        root["from_name"] = from->second;
        root["to_name"] = to->second;
        root["from_id"] = KeyUtils::toStrKey(fromAccount.getPublicKey());
        root["to_id"] = KeyUtils::toStrKey(toAccount.getPublicKey());
        root["amount"] = (Json::UInt64)paymentAmount;

        TransactionFramePtr txFrame;
        if (create != retMap.end() && create->second == "true")
        {
            txFrame = fromAccount.tx({createAccount(toAccount, paymentAmount)});
        }
        else
        {
            txFrame = fromAccount.tx({payment(toAccount, paymentAmount)});
        }

        switch (mApp.getHerder().recvTransaction(txFrame))
        {
        case Herder::TX_STATUS_PENDING:
            root["status"] = "pending";
            break;
        case Herder::TX_STATUS_DUPLICATE:
            root["status"] = "duplicate";
            break;
        case Herder::TX_STATUS_ERROR:
            root["status"] = "error";
            root["detail"] =
                xdr::xdr_to_string(txFrame->getResult().result.code());
            break;
        default:
            assert(false);
        }
    }
    else
    {
        root["status"] = "error";
        root["detail"] = "Bad HTTP GET: try something like: "
                         "testtx?from=root&to=bob&amount=1000000000";
    }
    retStr = root.toStyledString();
}

void
CommandHandler::fileNotFound(std::string const& params, std::string& retStr)
{
    retStr = "<b>Welcome to digitalbits-core!</b><p>\n";
    retStr += "supported commands:<p/>\n\n\n";

    retStr +=
        "<p><h1> /bans</h1>\n"
        "list current active bans\n\n"
        "</p><p><h1> /catchup?ledger=NNN[&mode=MODE]</h1>\n"
        "triggers the instance to catch up to ledger NNN from history; \n"
        "mode is either 'minimal' (the default, if omitted) or 'complete'.\n\n"
        "</p><p><h1> /checkdb</h1>\n"
        "triggers the instance to perform an integrity check of the database.\n\n"
        "</p><p><h1> /connect?peer=NAME&port=NNN</h1>\n"
        "triggers the instance to connect to peer NAME at port NNN.\n"
        "</p><p><h1> \n\n"
        "/droppeer?node=NODE_ID[&ban=D]</h1>\n"
        "drops peer identified by PEER_ID, when D is 1 the peer is also banned\n"
        "</p><p><h1> \n\n"
        "/generateload[?accounts=N&txs=M&txrate=(R|auto)]</h1>\n"
        "artificially generate load for testing; must be used with \n"
        "ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING set to true\n\n"
        "</p><p><h1> /help</h1>\n"
        "give a list of currently supported commands\n\n"
        "</p><p><h1> /info</h1>\n"
        "returns information about the server in JSON format (sync state, \n"
        "connected peers, etc)\n\n"
        "</p><p><h1> /ll?level=L[&partition=P]</h1>\n"
        "adjust the log level for partition P (or all if no partition is \n"
        "specified).<br>\n"
        "level is one of FATAL, ERROR, WARNING, INFO, DEBUG, VERBOSE, TRACE\n\n"
        "</p><p><h1> /logrotate</h1>\n"
        "rotate log files\n\n"
        "</p><p><h1> /manualclose</h1>\n"
        "close the current ledger; must be used with MANUAL_CLOSE set to true\n\n"
        "</p><p><h1> /metrics</h1>\n"
        "returns a snapshot of the metrics registry (for monitoring and \n"
        "debugging purpose)\n\n"
        "</p><p><h1> /peers</h1>\n"
        "returns the list of known peers in JSON format\n\n"
        "</p><p><h1> /quorum?[node=NODE_ID][&compact=true]</h1>\n"
        "returns information about the quorum for node NODE_ID (this node by\n"
        " default). NODE_ID is either a full key (`GABCD...`), an alias \n"
        "(`$name`) or an abbreviated ID(`@GABCD`).\n"
        "If compact is set, only returns a summary version.\n\n"
        "</p><p><h1> /scp?[limit=n]</h1>\n"
        "returns a JSON object with the internal state of the SCP engine for \n"
        "the last n (default 2) ledgers.\n\n"
        "</p><p><h1> /tx?blob=BASE64</h1>\n"
        "submit a transaction to the network.<br>\n"
        "blob is a base64 encoded XDR serialized 'TransactionEnvelope'<br>\n"
        "returns a JSON object<br>\n"
        "wasReceived: boolean, true if transaction was queued properly<br>\n"
        "result: base64 encoded, XDR serialized 'TransactionResult'<br>\n\n"
        "</p><p><h1> /upgrades?mode=(get|set|clear)&[upgradetime=DATETIME]&\n"
        "[basefee=NUM]&[basereserve=NUM]&[maxtxsize=NUM]&[protocolversion=NUM]\n"
        "</h1>\n"
        "gets, sets or clears upgrades.<br>\n"
        "When mode=set, upgradetime is a required date in the ISO 8601 \n"
        "date format (UTC) in the form 1970-01-01T00:00:00Z.<br>\n"
        "fee (uint32) This is what you would prefer the base fee to be. It is \n"
        "in stroops<br>\n"
        "basereserve (uint32) This is what you would prefer the base reserve \n"
        "to be. It is in stroops.<br>\n"
        "maxtxsize (uint32) This defines the maximum number of transactions \n"
        "to include in a ledger. When too many transactions are pending, \n"
        "surge pricing is applied. The instance picks the top maxtxsize\n"
        " transactions locally to be considered in the next ledger.Where \n"
        "transactions are ordered by transaction fee(lower fee transactions\n"
        " are held for later).<br>\n"
        "protocolversion (uint32) defines the protocol version to upgrade to.\n"
        " When specified it must match the protocol version supported by the\n"
        " node<br>\n\n"
        "</p><p><h1> /dropcursor?id=XYZ</h1> deletes the tracking cursor with \n"
        "identified by `id`. See `setcursor` for more information\n\n"
        "</p><p><h1> /setcursor?id=ID&cursor=N</h1> sets or creates a cursor \n"
        "identified by `ID` with value `N`. ID is an uppercase AlphaNum, N is \n"
        "an uint32 that represents the last ledger sequence number that the \n"
        "instance ID processed.\n\n"
        "Cursors are used by dependent services to tell digitalbits-core which \n"
        "data can be safely deleted by the instance.\n"
        "The data is historical data stored in the SQL tables such as \n"
        "txhistory or ledgerheaders.When all consumers processed the data for \n"
        "ledger sequence N the data can be safely removed by the instance.\n"
        "The actual deletion is performed by invoking the `maintenance` \n"
        "endpoint.\n\n"
        "</p><p><h1> /getcursor?[id=ID]</h1> gets the cursor identified by \n"
        "'ID'.  If ID is not defined then all cursors will be returned.\n\n"
        "</p><p><h1> /maintenance[?queue=true[&count=N]]</h1> Performs \n"
        "maintenance tasks on the instance.\n"
        "<ul><li><i>queue</i> performs deletion of queue data. Deletes at most \n"
        "count entries from each table (defaults to 50000). See setcursor for \n"
        "more information</li></ul>\n\n"
        "</p><p><h1> "
        "/unban?node=NODE_ID</h1>\n"
        "remove ban for PEER_ID"
        "</p>\n\n"

        "<br>\n\n";

    retStr += "<p>Have fun!</p>\n";
}

void
CommandHandler::manualClose(std::string const& params, std::string& retStr)
{
    if (mApp.manualClose())
    {
        retStr = "Forcing ledger to close...";
    }
    else
    {
        retStr =
            "Set MANUAL_CLOSE=true in the digitalbits-core.cfg if you want this "
            "behavior";
    }
}

template <typename T>
optional<T>
maybeParseNumParam(std::map<std::string, std::string> const& map,
                   std::string const& key, T& defaultVal)
{
    auto i = map.find(key);
    if (i != map.end())
    {
        std::stringstream str(i->second);
        str >> defaultVal;

        // Throw an error if not all bytes were loaded into `val`
        if (str.fail() || !str.eof())
        {
            std::string errorMsg =
                fmt::format("Failed to parse '{}' argument", key);
            throw std::runtime_error(errorMsg);
        }
        return make_optional<T>(defaultVal);
    }

    return nullopt<T>();
}

template <typename T>
T
parseNumParam(std::map<std::string, std::string> const& map,
              std::string const& key)
{
    T val;
    auto res = maybeParseNumParam(map, key, val);
    if (!res)
    {
        std::string errorMsg = fmt::format("'{}' argument is required!", key);
        throw std::runtime_error(errorMsg);
    }
    return val;
}

void
CommandHandler::generateLoad(std::string const& params, std::string& retStr)
{
    if (mApp.getConfig().ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING)
    {
        // Defaults are 200k accounts, 200k txs, 10 tx/s. This load-test will
        // therefore take 40k secs or about 12 hours.

        uint32_t nAccounts = 200000;
        uint32_t nTxs = 200000;
        uint32_t txRate = 10;
        bool autoRate = false;

        std::map<std::string, std::string> map;
        http::server::server::parseParams(params, map);

        maybeParseNumParam(map, "accounts", nAccounts);
        maybeParseNumParam(map, "txs", nTxs);

        {
            auto i = map.find("txrate");
            if (i != map.end() && i->second == std::string("auto"))
            {
                autoRate = true;
            }
            else
            {
                maybeParseNumParam(map, "txrate", txRate);
            }
        }

        double hours = ((nAccounts + nTxs) / txRate) / 3600.0;
        mApp.generateLoad(nAccounts, nTxs, txRate, autoRate);
        retStr = fmt::format(
            "Generating load: {:d} accounts, {:d} txs, {:d} tx/s = {:f} hours",
            nAccounts, nTxs, txRate, hours);
    }
    else
    {
        retStr = "Set ARTIFICIALLY_GENERATE_LOAD_FOR_TESTING=true in "
                 "the digitalbits-core.cfg if you want this behavior";
    }
}

void
CommandHandler::peers(std::string const&, std::string& retStr)
{
    Json::Value root;

    root["pending_peers"];
    int counter = 0;
    for (auto peer : mApp.getOverlayManager().getPendingPeers())
    {
        root["pending_peers"][counter]["ip"] = peer->getIP();
        root["pending_peers"][counter]["port"] =
            (int)peer->getRemoteListeningPort();

        counter++;
    }

    root["authenticated_peers"];
    counter = 0;
    for (auto peer : mApp.getOverlayManager().getAuthenticatedPeers())
    {
        root["authenticated_peers"][counter]["ip"] = peer.second->getIP();
        root["authenticated_peers"][counter]["port"] =
            (int)peer.second->getRemoteListeningPort();
        root["authenticated_peers"][counter]["ver"] =
            peer.second->getRemoteVersion();
        root["authenticated_peers"][counter]["olver"] =
            (int)peer.second->getRemoteOverlayVersion();
        root["authenticated_peers"][counter]["id"] =
            mApp.getConfig().toStrKey(peer.first);

        counter++;
    }

    retStr = root.toStyledString();
}

void
CommandHandler::info(std::string const&, std::string& retStr)
{
    retStr = mApp.getJsonInfo().toStyledString();
}

void
CommandHandler::metrics(std::string const& params, std::string& retStr)
{
    mApp.syncAllMetrics();
    medida::reporting::JsonReporter jr(mApp.getMetrics());
    retStr = jr.Report();
}

void
CommandHandler::logRotate(std::string const& params, std::string& retStr)
{
    retStr = "Log rotate...";

    Logging::rotate();
}

void
CommandHandler::catchup(std::string const& params, std::string& retStr)
{
    switch (mApp.getLedgerManager().getState())
    {
    case LedgerManager::LM_BOOTING_STATE:
        retStr = "Ledger Manager is still booting, try later";
        return;
    case LedgerManager::LM_CATCHING_UP_STATE:
        retStr = "Catchup already in progress";
        return;
    default:
        break;
    }

    uint32_t count = 0;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    uint32_t ledger = parseNumParam<uint32_t>(retMap, "ledger");

    auto modeP = retMap.find("mode");
    if (modeP != retMap.end())
    {
        if (modeP->second == std::string("complete"))
        {
            count = std::numeric_limits<uint32_t>::max();
        }
        else if (modeP->second == std::string("minimal"))
        {
            count = 0;
        }
        else if (modeP->second == std::string("recent"))
        {
            count = mApp.getConfig().CATCHUP_RECENT;
        }
        else
        {
            retStr = "Mode should be either 'minimal', 'recent' or 'complete'";
            return;
        }
    }

    mApp.getLedgerManager().startCatchUp({ledger, count}, true);
    retStr = (std::string("Started catchup to ledger ") +
              std::to_string(ledger) + std::string(" in mode ") +
              std::string(
                  count == std::numeric_limits<uint32_t>::max()
                      ? "CATCHUP_COMPLETE"
                      : (count != 0 ? "CATCHUP_RECENT" : "CATCHUP_MINIMAL")));
}

void
CommandHandler::checkdb(std::string const& params, std::string& retStr)
{
    mApp.checkDB();
    retStr = "CheckDB started.";
}

void
CommandHandler::connect(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    auto peerP = retMap.find("peer");
    auto portP = retMap.find("port");
    if (peerP != retMap.end() && portP != retMap.end())
    {
        std::stringstream str;
        str << peerP->second << ":" << portP->second;
        retStr = "Connect to: ";
        retStr += str.str();
        mApp.getOverlayManager().connectTo(str.str());
    }
    else
    {
        retStr = "Must specify a peer and port: connect&peer=PEER&port=PORT";
    }
}

void
CommandHandler::dropPeer(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    auto peerId = retMap.find("node");
    auto ban = retMap.find("ban");
    if (peerId != retMap.end())
    {
        auto found = false;
        NodeID n;
        if (mApp.getHerder().resolveNodeID(peerId->second, n))
        {
            auto peers = mApp.getOverlayManager().getAuthenticatedPeers();
            auto peer = peers.find(n);
            if (peer != peers.end())
            {
                mApp.getOverlayManager().dropPeer(peer->second.get());
                if (ban != retMap.end() && ban->second == "1")
                {
                    retStr = "Drop and ban peer: ";
                    mApp.getBanManager().banNode(n);
                }
                else
                    retStr = "Drop peer: ";

                retStr += peerId->second;
                found = true;
            }
        }

        if (!found)
        {
            retStr = "Peer ";
            retStr += peerId->second;
            retStr += " not found";
        }
    }
    else
    {
        retStr = "Must specify at least peer id: droppeer?node=NODE_ID";
    }
}

void
CommandHandler::bans(std::string const& params, std::string& retStr)
{
    Json::Value root;

    root["bans"];
    int counter = 0;
    for (auto ban : mApp.getBanManager().getBans())
    {
        root["bans"][counter] = ban;

        counter++;
    }

    retStr = root.toStyledString();
}

void
CommandHandler::unban(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    auto peerId = retMap.find("node");
    if (peerId != retMap.end())
    {
        NodeID n;
        if (mApp.getHerder().resolveNodeID(peerId->second, n))
        {
            retStr = "Unban peer: ";
            retStr += peerId->second;
            mApp.getBanManager().unbanNode(n);
        }
        else
        {
            retStr = "Peer ";
            retStr += peerId->second;
            retStr += " not found";
        }
    }
    else
    {
        retStr = "Must specify at least peer id: unban?node=NODE_ID";
    }
}

void
CommandHandler::upgrades(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);
    auto s = retMap["mode"];
    if (s.empty())
    {
        retStr = "mode required";
        return;
    }
    if (s == "get")
    {
        retStr = mApp.getHerder().getUpgradesJson();
    }
    else if (s == "set")
    {
        Upgrades::UpgradeParameters p;

        auto upgradeTime = retMap["upgradetime"];
        std::tm tm;
        try
        {
            tm = VirtualClock::isoStringToTm(upgradeTime);
        }
        catch (std::exception)
        {
            retStr =
                fmt::format("could not parse upgradetime: '{}'", upgradeTime);
            return;
        }
        p.mUpgradeTime = VirtualClock::tmToPoint(tm);

        uint32 baseFee;
        uint32 baseReserve;
        uint32 maxTxSize;
        uint32 protocolVersion;

        p.mBaseFee = maybeParseNumParam(retMap, "basefee", baseFee);
        p.mBaseReserve = maybeParseNumParam(retMap, "basereserve", baseReserve);
        p.mMaxTxSize = maybeParseNumParam(retMap, "maxtxsize", maxTxSize);
        p.mProtocolVersion =
            maybeParseNumParam(retMap, "protocolversion", protocolVersion);

        mApp.getHerder().setUpgrades(p);
    }
    else if (s == "clear")
    {
        Upgrades::UpgradeParameters p;
        mApp.getHerder().setUpgrades(p);
    }
    else
    {
        retStr = fmt::format("Unknown mode: {}", s);
    }
}

void
CommandHandler::quorum(std::string const& params, std::string& retStr)
{
    Json::Value root;
    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    NodeID n;

    std::string nID = retMap["node"];

    if (nID.empty())
    {
        n = mApp.getConfig().NODE_SEED.getPublicKey();
    }
    else
    {
        if (!mApp.getHerder().resolveNodeID(nID, n))
        {
            throw std::invalid_argument("unknown name");
        }
    }

    mApp.getHerder().dumpQuorumInfo(root, n, retMap["compact"] == "true");

    retStr = root.toStyledString();
}

void
CommandHandler::scpInfo(std::string const& params, std::string& retStr)
{
    Json::Value root;

    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    size_t lim = 2;
    maybeParseNumParam(retMap, "limit", lim);

    mApp.getHerder().dumpInfo(root, lim);

    retStr = root.toStyledString();
}

// "Must specify a log level: ll?level=<level>&partition=<name>";
void
CommandHandler::ll(std::string const& params, std::string& retStr)
{
    Json::Value root;

    std::map<std::string, std::string> retMap;
    http::server::server::parseParams(params, retMap);

    std::string levelStr = retMap["level"];
    std::string partition = retMap["partition"];
    if (!levelStr.size())
    {
        root["Fs"] = Logging::getStringFromLL(Logging::getLogLevel("Fs"));
        root["SCP"] = Logging::getStringFromLL(Logging::getLogLevel("SCP"));
        root["Bucket"] =
            Logging::getStringFromLL(Logging::getLogLevel("Bucket"));
        root["Database"] =
            Logging::getStringFromLL(Logging::getLogLevel("Database"));
        root["History"] =
            Logging::getStringFromLL(Logging::getLogLevel("History"));
        root["Process"] =
            Logging::getStringFromLL(Logging::getLogLevel("Process"));
        root["Ledger"] =
            Logging::getStringFromLL(Logging::getLogLevel("Ledger"));
        root["Overlay"] =
            Logging::getStringFromLL(Logging::getLogLevel("Overlay"));
        root["Herder"] =
            Logging::getStringFromLL(Logging::getLogLevel("Herder"));
        root["Tx"] = Logging::getStringFromLL(Logging::getLogLevel("Tx"));
    }
    else
    {
        el::Level level = Logging::getLLfromString(levelStr);
        if (partition.size())
        {
            Logging::setLogLevel(level, partition.c_str());
            root[partition] = Logging::getStringFromLL(level);
        }
        else
        {
            Logging::setLogLevel(level, nullptr);
            root["Global"] = Logging::getStringFromLL(level);
        }
    }

    retStr = root.toStyledString();
}

static const char* TX_STATUS_STRING[Herder::TX_STATUS_COUNT] = {
    "PENDING", "DUPLICATE", "ERROR"};

void
CommandHandler::tx(std::string const& params, std::string& retStr)
{
    std::ostringstream output;

    const std::string prefix("?blob=");
    if (params.compare(0, prefix.size(), prefix) == 0)
    {
        TransactionEnvelope envelope;
        std::string blob = params.substr(prefix.size());
        std::vector<uint8_t> binBlob;
        bn::decode_b64(blob, binBlob);

        xdr::xdr_from_opaque(binBlob, envelope);
        TransactionFramePtr transaction =
            TransactionFrame::makeTransactionFromWire(mApp.getNetworkID(),
                                                      envelope);
        if (transaction)
        {
            // add it to our current set
            // and make sure it is valid
            Herder::TransactionSubmitStatus status =
                mApp.getHerder().recvTransaction(transaction);

            if (status == Herder::TX_STATUS_PENDING)
            {
                StellarMessage msg;
                msg.type(TRANSACTION);
                msg.transaction() = envelope;
                mApp.getOverlayManager().broadcastMessage(msg);
            }

            output << "{"
                   << "\"status\": "
                   << "\"" << TX_STATUS_STRING[status] << "\"";
            if (status == Herder::TX_STATUS_ERROR)
            {
                std::string resultBase64;
                auto resultBin = xdr::xdr_to_opaque(transaction->getResult());
                resultBase64.reserve(bn::encoded_size64(resultBin.size()) + 1);
                resultBase64 = bn::encode_b64(resultBin);

                output << " , \"error\": \"" << resultBase64 << "\"";
            }
            output << "}";
        }
    }
    else
    {
        throw std::invalid_argument("Must specify a tx blob: tx?blob=<tx in "
                                    "xdr format>\"}");
    }

    retStr = output.str();
}

void
CommandHandler::dropcursor(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);
    std::string const& id = map["id"];

    if (!ExternalQueue::validateResourceID(id))
    {
        retStr = "Invalid resource id";
    }
    else
    {
        ExternalQueue ps(mApp);
        ps.deleteCursor(id);
        retStr = "Done";
    }
}

void
CommandHandler::setcursor(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);
    std::string const& id = map["id"];

    uint32 cursor = parseNumParam<uint32>(map, "cursor");

    if (!ExternalQueue::validateResourceID(id))
    {
        retStr = "Invalid resource id";
    }
    else
    {
        ExternalQueue ps(mApp);
        ps.setCursorForResource(id, cursor);
        retStr = "Done";
    }
}

void
CommandHandler::getcursor(std::string const& params, std::string& retStr)
{
    Json::Value root;
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);
    std::string const& id = map["id"];

    // the decision was made not to check validity here
    // because there are subsequent checks for that in
    // ExternalQueue and if an exception is thrown for
    // validity there, the ret format is technically more
    // correct for the mime type
    ExternalQueue ps(mApp);
    std::map<std::string, uint32> curMap;
    int counter = 0;
    ps.getCursorForResource(id, curMap);
    root["cursors"][0];
    for (auto cursor : curMap)
    {
        root["cursors"][counter]["id"] = cursor.first;
        root["cursors"][counter]["cursor"] = cursor.second;
        counter++;
    }

    retStr = root.toStyledString();
}

void
CommandHandler::maintenance(std::string const& params, std::string& retStr)
{
    std::map<std::string, std::string> map;
    http::server::server::parseParams(params, map);
    if (map["queue"] == "true")
    {
        uint32_t count = 50000;
        maybeParseNumParam(map, "count", count);

        mApp.getMaintainer().performMaintenance(count);
        retStr = "Done";
    }
    else
    {
        retStr = "No work performed";
    }
}
}
