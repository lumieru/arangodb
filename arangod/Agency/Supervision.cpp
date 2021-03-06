////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#include "Supervision.h"

#include "Agent.h"
#include "CleanOutServer.h"
#include "FailedLeader.h"
#include "FailedServer.h"
#include "MoveShard.h"
#include "Job.h"
#include "Store.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Basics/ConditionLocker.h"
#include "VocBase/server.h"

#include <thread>
using namespace arangodb;

using namespace arangodb::consensus;
using namespace arangodb::application_features;

std::string Supervision::_agencyPrefix = "/arango";

Supervision::Supervision()
    : arangodb::Thread("Supervision"),
      _agent(nullptr),
      _snapshot("Supervision"),
      _frequency(5),
      _gracePeriod(15),
      _jobId(0),
      _jobIdMax(0),
      _selfShutdown(false) {}

Supervision::~Supervision() { shutdown(); };

void Supervision::wakeUp() {
  _cv.signal();
}

static std::string const syncPrefix = "/Sync/ServerStates/";
static std::string const healthPrefix = "/Supervision/Health/";
static std::string const planDBServersPrefix = "/Plan/DBServers";
static std::string const planCoordinatorsPrefix = "/Plan/Coordinators";
static std::string const currentServersRegisteredPrefix 
    = "/Current/ServersRegistered";
static std::string const foxxmaster = "/Current/Foxxmaster";

std::vector<check_t> Supervision::checkDBServers() {
  std::vector<check_t> ret;
  Node::Children const& machinesPlanned =
      _snapshot(planDBServersPrefix).children();
  Node::Children const serversRegistered =
      _snapshot(currentServersRegisteredPrefix).children();

  std::vector<std::string> todelete;
  for (auto const& machine : _snapshot(healthPrefix).children()) {
    if (machine.first.substr(0,2) == "DB") {
      todelete.push_back(machine.first);
    }
  }

  for (auto const& machine : machinesPlanned) {

    bool good = false;
    std::string lastHeartbeatTime, lastHeartbeatAcked, lastStatus, heartbeatTime,
      heartbeatStatus, serverID;

    serverID        = machine.first;
    heartbeatTime   = _snapshot(syncPrefix + serverID + "/time").toJson();
    heartbeatStatus = _snapshot(syncPrefix + serverID + "/status").toJson();

    todelete.erase(
      std::remove(todelete.begin(), todelete.end(), serverID), todelete.end());
    
    try {           // Existing
      lastHeartbeatTime =
        _snapshot(healthPrefix + serverID + "/LastHeartbeatSent").toJson();
      lastHeartbeatAcked =
        _snapshot(healthPrefix + serverID + "/LastHeartbeatAcked").toJson();
      lastStatus = _snapshot(healthPrefix + serverID + "/Status").toJson();
      if (lastHeartbeatTime != heartbeatTime) { // Update
        good = true;
      }
    } catch (...) { // New server
      good = true;
    }
    
    query_t report = std::make_shared<Builder>();
    report->openArray();
    report->openArray();
    report->openObject();
    report->add(_agencyPrefix + healthPrefix + serverID,
                VPackValue(VPackValueType::Object));
    report->add("LastHeartbeatSent", VPackValue(heartbeatTime));
    report->add("LastHeartbeatStatus", VPackValue(heartbeatStatus));
    report->add("Role", VPackValue("DBServer"));
    auto endpoint = serversRegistered.find(serverID);
    if (endpoint != serversRegistered.end()) {
      endpoint = endpoint->second->children().find("endpoint");
      if (endpoint != endpoint->second->children().end()) {
        if (endpoint->second->children().size() == 0) {
          VPackSlice epString = endpoint->second->slice();
          if (epString.isString()) {
            report->add("Endpoint", epString);
          }
        }
      }
    }
      
    if (good) {
      report->add("LastHeartbeatAcked",
                  VPackValue(
                    timepointToString(std::chrono::system_clock::now())));
      report->add("Status", VPackValue(Supervision::HEALTH_STATUS_GOOD));
    } else {
      std::chrono::seconds t{0};
      t = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now()-stringToTimepoint(lastHeartbeatAcked));
      if (t.count() > _gracePeriod) {  // Failure
        if (lastStatus == "BAD") {
          report->add("Status", VPackValue("FAILED"));
          FailedServer fsj(_snapshot, _agent, std::to_string(_jobId++),
                           "supervision", _agencyPrefix, serverID);
        }
      } else {
        report->add("Status", VPackValue("BAD"));
      }
    }
    
    report->close();
    report->close();
    report->close();
    report->close();
    if (!this->isStopping()) {
      _agent->write(report);
    }
    
  }

  if (!todelete.empty()) {
    query_t del = std::make_shared<Builder>();
    del->openArray();
    del->openArray();
    del->openObject();
    for (auto const& srv : todelete) {
      del->add(_agencyPrefix + healthPrefix + srv,
               VPackValue(VPackValueType::Object));
      del->add("op", VPackValue("delete"));
      del->close();
    }
    del->close(); del->close(); del->close();
    _agent->write(del);
  }
  
  return ret;
}

std::vector<check_t> Supervision::checkCoordinators() {
  std::vector<check_t> ret;
  Node::Children const& machinesPlanned =
      _snapshot(planCoordinatorsPrefix).children();
  Node::Children const serversRegistered =
      _snapshot(currentServersRegisteredPrefix).children();

  std::string currentFoxxmaster;
  try {
    currentFoxxmaster = _snapshot(foxxmaster).getString();
  } catch (...) {
  }
  
  std::string goodServerId;
  bool foxxmasterOk = false;
  std::vector<std::string> todelete;
  for (auto const& machine : _snapshot(healthPrefix).children()) {
    if (machine.first.substr(0,2) == "Co") {
      todelete.push_back(machine.first);
    }
  }

  for (auto const& machine : machinesPlanned) {

    bool good = false;
    std::string lastHeartbeatTime, lastHeartbeatAcked, lastStatus, heartbeatTime,
      heartbeatStatus, serverID;

    serverID        = machine.first;
    heartbeatTime   = _snapshot(syncPrefix + serverID + "/time").toJson();
    heartbeatStatus = _snapshot(syncPrefix + serverID + "/status").toJson();
    
    todelete.erase(
      std::remove(todelete.begin(), todelete.end(), serverID), todelete.end());
    
    try {           // Existing
      lastHeartbeatTime =
        _snapshot(healthPrefix + serverID + "/LastHeartbeatSent").toJson();
      lastStatus = _snapshot(healthPrefix + serverID + "/Status").toJson();
      if (lastHeartbeatTime != heartbeatTime) { // Update
        good = true;
      }
    } catch (...) { // New server
      good = true;
    }
    
    query_t report = std::make_shared<Builder>();
    report->openArray();
    report->openArray();
    report->openObject();
    report->add(_agencyPrefix + healthPrefix + serverID,
                VPackValue(VPackValueType::Object));
    report->add("LastHeartbeatSent", VPackValue(heartbeatTime));
    report->add("LastHeartbeatStatus", VPackValue(heartbeatStatus));
    report->add("Role", VPackValue("Coordinator"));
    auto endpoint = serversRegistered.find(serverID);
    if (endpoint != serversRegistered.end()) {
      endpoint = endpoint->second->children().find("endpoint");
      if (endpoint != endpoint->second->children().end()) {
        if (endpoint->second->children().size() == 0) {
          VPackSlice epString = endpoint->second->slice();
          if (epString.isString()) {
            report->add("Endpoint", epString);
          }
        }
      }
    }
    
    if (good) {
      if (goodServerId.empty()) {
        goodServerId = serverID;
      }
      if (serverID == currentFoxxmaster) {
        foxxmasterOk = true;
      }
      report->add("LastHeartbeatAcked",
                  VPackValue(
                    timepointToString(std::chrono::system_clock::now())));
      report->add("Status", VPackValue(Supervision::HEALTH_STATUS_GOOD));
    } else {
      std::chrono::seconds t{0};
      t = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now()-stringToTimepoint(lastHeartbeatAcked));
      if (t.count() > _gracePeriod) {  // Failure
        if (lastStatus == Supervision::HEALTH_STATUS_BAD) {
          report->add("Status", VPackValue(Supervision::HEALTH_STATUS_FAILED));
        }
      } else {
        report->add("Status", VPackValue(Supervision::HEALTH_STATUS_BAD));
      }
    }

    report->close();
    report->close();
    report->close();
    report->close();
    if (!this->isStopping()) {
      _agent->write(report);
    }
      
  }

  if (!todelete.empty()) {
    query_t del = std::make_shared<Builder>();
    del->openArray();
    del->openArray();
    del->openObject();
    for (auto const& srv : todelete) {
      del->add(_agencyPrefix + healthPrefix + srv,
               VPackValue(VPackValueType::Object));
      del->add("op", VPackValue("delete"));
      del->close();
    }
    del->close(); del->close(); del->close();
    _agent->write(del);
  }

  if (!foxxmasterOk && !goodServerId.empty()) {
    query_t create = std::make_shared<Builder>();
    create->openArray();
    create->openArray();
    create->openObject();
    create->add(_agencyPrefix + foxxmaster, VPackValue(goodServerId));
    create->close();
    create->close();
    create->close();
    
    _agent->write(create);
  }
  
  return ret;

}

bool Supervision::updateSnapshot() {

  if (_agent == nullptr || this->isStopping()) {
    return false;
  }
  _snapshot = _agent->readDB().get(_agencyPrefix);
  return true;
}

bool Supervision::doChecks() {

  checkDBServers();
  checkCoordinators();
  return true;
  
}

void Supervision::run() {
  CONDITION_LOCKER(guard, _cv);
  TRI_ASSERT(_agent != nullptr);
  
  // Get agency prefix after cluster init
  if (_jobId == 0) {
    // We need the agency prefix to work, but it is only initialized by
    // some other server in the cluster. Since the supervision does not
    // make sense at all without other ArangoDB servers, we wait pretty
    // long here before giving up:
    if (!updateAgencyPrefix(1000, 1)) {
      LOG_TOPIC(ERR, Logger::AGENCY)
        << "Cannot get prefix from Agency. Stopping supervision for good.";
      return;
    }
  }
  
  while (!this->isStopping()) {
    updateSnapshot();
    // mop: always do health checks so shutdown is able to detect if a server failed otherwise
    if (_agent->leading()) {
      doChecks();
    }

    if (isShuttingDown()) {
      handleShutdown();
    } else if (_selfShutdown) {
      ApplicationServer::server->beginShutdown();
    } else if (_agent->leading()) {
      if (!handleJobs()) {
        break;
      }
    }
    _cv.wait(_frequency * 1000000);
  }
}

bool Supervision::isShuttingDown() {
  try {
    return _snapshot("/Shutdown").getBool();
  } catch (...) {
    return false;
  }
}

bool Supervision::serverGood(const std::string& serverName) {
  try {
    const std::string serverStatus(healthPrefix + serverName + "/Status");
    const std::string status = _snapshot(serverStatus).getString();
    return status == Supervision::HEALTH_STATUS_GOOD;
  } catch (...) {
    return false;
  }
}

void Supervision::handleShutdown() {
  _selfShutdown = true;
  LOG_TOPIC(DEBUG, Logger::AGENCY) << "Waiting for clients to shut down";
  Node::Children const& serversRegistered = _snapshot(currentServersRegisteredPrefix).children();
  bool serversCleared = true;
  for (auto const& server : serversRegistered) {
    if (server.first == "Version") {
      continue;
    }
     
    LOG_TOPIC(DEBUG, Logger::AGENCY)
      << "Waiting for " << server.first << " to shutdown";

    if (!serverGood(server.first)) {
      LOG_TOPIC(WARN, Logger::AGENCY)
        << "Server " << server.first << " did not shutdown properly it seems!";
      continue;
    }
    serversCleared = false;
  }

  if (serversCleared) {
    if (_agent->leading()) {

      query_t del = std::make_shared<Builder>();
      del->openArray();
      del->openArray();
      del->openObject();
      del->add(_agencyPrefix + "/Shutdown", VPackValue(VPackValueType::Object));
      del->add("op", VPackValue("delete"));
      del->close();
      del->close(); del->close(); del->close();
      auto result = _agent->write(del);
      if (result.indices.size() != 1) {
        LOG(ERR) << "Invalid resultsize of " << result.indices.size() << " found during shutdown";
      } else {
        if (!_agent->waitFor(result.indices.at(0))) {
          LOG(ERR) << "Result was not written to followers during shutdown";
        }
      }
    }
  }
}

bool Supervision::handleJobs() {
  // Get bunch of job IDs from agency for future jobs
  if (_jobId == 0 || _jobId == _jobIdMax) {
    getUniqueIds();  // cannot fail but only hang
  }

  // Do supervision
  shrinkCluster();
  workJobs();
  
  return true;
}

void Supervision::workJobs() {
  Node::Children const& todos = _snapshot(toDoPrefix).children();
  Node::Children const& pends = _snapshot(pendingPrefix).children();

  for (auto const& todoEnt : todos) {
    Node const& job = *todoEnt.second;
    
    std::string jobType = job("type").getString(),
      jobId = job("jobId").getString(),
      creator = job("creator").getString();
    if (jobType == "failedServer") {
      FailedServer fs(_snapshot, _agent, jobId, creator, _agencyPrefix);
    } else if (jobType == "cleanOutServer") {
      CleanOutServer cos(_snapshot, _agent, jobId, creator, _agencyPrefix);
    } else if (jobType == "moveShard") {
      MoveShard mv(_snapshot, _agent, jobId, creator, _agencyPrefix);
    } else if (jobType == "failedLeader") {
      FailedLeader fl(_snapshot, _agent, jobId, creator, _agencyPrefix);
    }
  }

  for (auto const& pendEnt : pends) {
    Node const& job = *pendEnt.second;
    
    std::string jobType = job("type").getString(),
      jobId = job("jobId").getString(),
      creator = job("creator").getString();
    if (jobType == "failedServer") {
      FailedServer fs(_snapshot, _agent, jobId, creator, _agencyPrefix);
    } else if (jobType == "cleanOutServer") {
      CleanOutServer cos(_snapshot, _agent, jobId, creator, _agencyPrefix);
    } else if (jobType == "moveShard") {
      MoveShard mv(_snapshot, _agent, jobId, creator, _agencyPrefix);
    } else if (jobType == "failedLeader") {
      FailedLeader fl(_snapshot, _agent, jobId, creator, _agencyPrefix);
    }
  }
  
}


// Shrink cluster if applicable
void Supervision::shrinkCluster () {

  // Get servers from plan
  std::vector<std::string> availServers; 
  Node::Children const& dbservers = _snapshot("/Plan/DBServers").children();
  for (auto const& srv : dbservers) {
    availServers.push_back(srv.first);
  }

  size_t targetNumDBServers;
  try {
    targetNumDBServers = _snapshot("/Target/NumberOfDBServers").getUInt();
  } catch (std::exception const& e) {
    LOG_TOPIC(DEBUG, Logger::AGENCY)
      << "Targeted number of DB servers not set yet: " << e.what();
    return;
  }
  
  // If there are any cleanOutServer jobs todo or pending do nothing
  Node::Children const& todos = _snapshot(toDoPrefix).children();
  Node::Children const& pends = _snapshot(pendingPrefix).children();
  
  for (auto const& job : todos) {
    try {
      if ((*job.second)("type").getString() == "cleanOutServer") {
        return;
      }
    } catch (std::exception const& e) {
      LOG_TOPIC(WARN, Logger::AGENCY)
        << "Failed to get job type of job " << job.first << ": " << e.what();
      return;
    }
  }
  
  for (auto const& job : pends) {
    try {
      if ((*job.second)("type").getString() == "cleanOutServer") {
        return;
      }
    } catch (std::exception const& e) {
      LOG_TOPIC(WARN, Logger::AGENCY)
        << "Failed to get job type of job " << job.first << ": " << e.what();
      return;
    }
  }
  
  // Remove cleaned from ist 
  if (_snapshot.exists("/Target/CleanedServers").size()==2) {
    for (auto const& srv :
           VPackArrayIterator(_snapshot("/Target/CleanedServers").slice())) {
      availServers.erase(
        std::remove(availServers.begin(), availServers.end(), srv.copyString()),
        availServers.end());
    }
  }

  // Only if number of servers in target is smaller than the available
  if (targetNumDBServers < availServers.size()) {

    // Minimum 1 DB server must remain
    if (availServers.size() == 1) {
      LOG_TOPIC(DEBUG, Logger::AGENCY) <<
        "Only one db server left for operation";
      return;
    }

    // Find greatest replication factor among all collections
    uint64_t maxReplFact = 1;
    Node::Children const& databases = _snapshot("/Plan/Collections").children();
    for (auto const& database : databases) {
      for (auto const& collptr : database.second->children()) {
        try {
          uint64_t replFact = (*collptr.second)("replicationFactor").getUInt();
          if (replFact > maxReplFact) {
            maxReplFact = replFact;
          }
        } catch (std::exception const&) {
          LOG_TOPIC(DEBUG, Logger::AGENCY) << "Cannot retrieve replication " <<
            "factor for collection " << collptr.first;
          return;
        }
      }
    }

    // If max number of replications is small than that of available
    if (maxReplFact < availServers.size()) {

      // Sort servers by name
      std::sort(availServers.begin(), availServers.end());
      
      // Clean out as long as number of available servers is bigger
      // than maxReplFactor and bigger than targeted number of db servers
      if (availServers.size() > maxReplFact &&
          availServers.size() > targetNumDBServers) {
        
        // Schedule last server for cleanout
        CleanOutServer(_snapshot, _agent, std::to_string(_jobId++),
                       "supervision", _agencyPrefix, availServers.back());
      }
      
    }
    
  }

}


// Start thread
bool Supervision::start() {
  Thread::start();
  return true;
}

// Start thread with agent
bool Supervision::start(Agent* agent) {
  _agent = agent;
  _frequency = static_cast<long>(_agent->config().supervisionFrequency());

  return start();
}

// Get agency prefix fron agency
bool Supervision::updateAgencyPrefix (size_t nTries, int intervalSec) {

  // Try nTries to get agency's prefix in intervals 
  while (!this->isStopping()) {
    _snapshot = _agent->readDB().get("/");
    if (_snapshot.children().size() > 0) {
      _agencyPrefix = std::string("/") + _snapshot.children().begin()->first;
      LOG_TOPIC(DEBUG, Logger::AGENCY) << "Agency prefix is " << _agencyPrefix;
      return true;
    }
    std::this_thread::sleep_for (std::chrono::seconds(intervalSec));
  }

  // Stand-alone agency
  return false;
  
}

static std::string const syncLatest = "/Sync/LatestID";
// Get bunch of cluster's unique ids from agency 
void Supervision::getUniqueIds() {
  uint64_t latestId;
  // Run forever, supervision does not make sense before the agency data
  // is initialized by some other server...
  while (!this->isStopping()) {
    try {
      latestId = std::stoul(
        _agent->readDB().get(_agencyPrefix + "/Sync/LatestID").slice().toJson());
    } catch (...) {
      std::this_thread::sleep_for (std::chrono::seconds(1));
      continue;
    }

    Builder uniq;
    uniq.openArray();
    uniq.openObject();
    uniq.add(_agencyPrefix + syncLatest, VPackValue(latestId + 100000)); // new 
    uniq.close();
    uniq.openObject();
    uniq.add(_agencyPrefix + syncLatest, VPackValue(latestId));      // precond
    uniq.close();
    uniq.close();

    auto result = transact(_agent, uniq);

    if (!result.accepted || result.indices.empty()) {
      LOG_TOPIC(DEBUG, Logger::AGENCY)
        << "We have lost agency leadership. Stopping any supervision processing "
        << __FILE__ << __LINE__;
      return;
    }
    
    if (result.indices[0]) {
      _agent->waitFor(result.indices[0]);
      _jobId = latestId;
      _jobIdMax = latestId + 100000;
      return;
    }
  }
}

void Supervision::updateFromAgency() {
  auto const& jobsPending =
      _snapshot("/Supervision/Jobs/Pending").children();

  for (auto const& jobent : jobsPending) {
    auto const& job = *(jobent.second);

    LOG_TOPIC(WARN, Logger::AGENCY)
      << job.name() << " " << job("failed").toJson() << job("");
  }
}

void Supervision::beginShutdown() {
  // Personal hygiene
  Thread::beginShutdown();

  CONDITION_LOCKER(guard, _cv);
  guard.broadcast();

}

Store const& Supervision::store() const {
  return _agent->readDB();
}
