/*
 The MIT License (MIT)

 Copyright (c) [2016] [BTC.COM]

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/

#include "StratumSessionGrin.h"

#include "StratumGrin.h"
#include "StratumMinerGrin.h"
#include "StratumServerGrin.h"

#include "DiffController.h"

#include <boost/make_unique.hpp>

namespace {

class ScopedMethodReset {
public:
  ScopedMethodReset(string &currentMethod, const string &method) : currentMethod_{currentMethod} {
    currentMethod.assign(method);
  }

  ~ScopedMethodReset() {
    currentMethod_.clear();
  }

private:
  string &currentMethod_;
};

enum class GrinErrorCode {
  UNAUTHORIZED = -32500,
  LOW_DIFFICULTY = -32501,
  INVALID_SOLUTION = -32502,
  JOB_NOT_FOUND = -32503,
  INVALID_REQUEST = -32600,
  ILLEGAL_METHOD = -32601,
};

std::pair<GrinErrorCode, const char *> StratumStatusToGrinError(int code) {
  switch (code) {
  case StratumStatus::UNAUTHORIZED:
    return std::make_pair(GrinErrorCode::UNAUTHORIZED, "Login first");
  case StratumStatus::LOW_DIFFICULTY:
    return std::make_pair(GrinErrorCode::LOW_DIFFICULTY, "Share rejected due to low difficulty");
  case StratumStatus::INVALID_SOLUTION:
    return std::make_pair(GrinErrorCode::INVALID_SOLUTION, "Failed to validate solution");
  case StratumStatus::JOB_NOT_FOUND:
    return std::make_pair(GrinErrorCode::JOB_NOT_FOUND, "Solution Submitted too late");
  case StratumStatus::ILLEGAL_METHOD:
    return std::make_pair(GrinErrorCode::ILLEGAL_METHOD, "Method not found");
  default:
    return std::make_pair(GrinErrorCode::INVALID_REQUEST, "Invalid request");
  }
}

}

StratumSessionGrin::StratumSessionGrin(
  StratumServerGrin &server,
  struct bufferevent *bev,
  struct sockaddr *saddr,
  uint32_t sessionId)
  : StratumSessionBase{server, bev, saddr, sessionId}
  , currentDifficulty_{0} {
}

void StratumSessionGrin::sendSetDifficulty(LocalJob &localJob, uint64_t difficulty) {
  currentDifficulty_ = difficulty;
}

void StratumSessionGrin::sendMiningNotify(shared_ptr<StratumJobEx> exJobPtr, bool isFirstJob) {
  sendMiningNotifyWithId(exJobPtr, "");
}

std::unique_ptr<StratumMiner> StratumSessionGrin::createMiner(
  const std::string &clientAgent,
  const std::string &workerName,
  int64_t workerId) {
  return boost::make_unique<StratumMinerGrin>(
    *this,
    *getServer().defaultDifficultyController_,
    clientAgent,
    workerName,
    workerId);
}

void StratumSessionGrin::responseError(const string &idStr, int code) {
  auto p = StratumStatusToGrinError(code);
  auto s = Strings::Format(
    "{\"id\":%s"
    ",\"jsonrpc\":\"2.0\""
    ",\"method\":\"%s\""
    ",\"error\":"
    "{\"code\":%d"
    ",\"message\":\"%s\""
    "}}\n",
    idStr.empty() ? "null" : idStr.c_str(),
    currentMethod_.empty() ? "null" : currentMethod_.c_str(),
    p.first,
    p.second);
  sendData(s);
}

void StratumSessionGrin::responseTrue(const string &idStr) {
  const string s = Strings::Format(
    "{\"id\":%s"
    ",\"jsonrpc\":\"2.0\""
    ",\"method\":\"%s\""
    ",\"result\":\"ok\""
    ",\"error\":null"
    "}\n",
    idStr.empty() ? "null" : idStr.c_str(),
    currentMethod_.empty() ? "null" : currentMethod_.c_str());
  sendData(s);
}

bool StratumSessionGrin::validate(const JsonNode &jmethod, const JsonNode &jparams, const JsonNode &jroot) {
  if (jmethod.type() == Utilities::JS::type::Str && jmethod.size() != 0) {
    return true;
  }
  return false;
}

void StratumSessionGrin::handleRequest(
  const std::string &idStr,
  const std::string &method,
  const JsonNode &jparams,
  const JsonNode &jroot) {
  ScopedMethodReset methodReset{currentMethod_, method};
  if (method == "login") {
    handleRequest_Authorize(idStr, jparams);
  } if (method == "getjobtemplate") {
    handleRequest_GetJobTemplate(idStr);
  } if (method == "keepalive") {
    responseTrue(idStr);
  } else if (dispatcher_) {
    dispatcher_->handleRequest(idStr, method, jparams, jroot);
  }
}

void StratumSessionGrin::handleRequest_Authorize(
  const string &idStr,
  const JsonNode &jparams) {
  // const type cannot access string indexed object member
  JsonNode &jsonParams = const_cast<JsonNode &>(jparams);

  string fullName;
  string password;
  if (jsonParams["login"].type() == Utilities::JS::type::Str) {
    fullName = jsonParams["login"].str();
  }
  if (jsonParams["pass"].type() == Utilities::JS::type::Str) {
    password = jsonParams["pass"].str();
  }

  checkUserAndPwd(idStr, fullName, password);

  string clientAgent;
  if (jsonParams["agent"].type() == Utilities::JS::type::Str) {
    fullName = jsonParams["agent"].str();
  }
  setClientAgent(clientAgent);

  return;
}

void StratumSessionGrin::handleRequest_GetJobTemplate(const std::string &idStr) {
  sendMiningNotifyWithId(getServer().GetJobRepository(getChainId())->getLatestStratumJobEx(), idStr.empty() ? "null" : idStr);
}



void StratumSessionGrin::sendMiningNotifyWithId(shared_ptr<StratumJobEx> exJobPtr, const std::string &idStr) {
  if (state_ < AUTHENTICATED || exJobPtr == nullptr) {
    LOG(ERROR) << "sendMiningNotify failed, state: " << state_;
    return;
  }

  StratumJobGrin *job = dynamic_cast<StratumJobGrin *>(exJobPtr->sjob_);
  if (nullptr == job) {
    return;
  }

  // Adjust minimal difficulty to fit secondary scaling, otherwise the hash target may has no leading zeros
  dispatcher_->setMinDiff(job->prePow_.secondaryScaling.value() * 2);

  uint32_t prePowHash = djb2(job->prePowStr_.c_str());
  auto ljob = findLocalJob(prePowHash);
  // create a new local job if not exists
  if (ljob == nullptr) {
    ljob = &addLocalJob(exJobPtr->chainId_, job->jobId_, prePowHash);
  } else {
    dispatcher_->addLocalJob(*ljob);
  }

  DLOG(INFO) << "new stratum job mining.notify: share difficulty=" << currentDifficulty_;
  string strNotify = Strings::Format(
    "{\"id\":%s"
    ",\"jsonrpc\":\"2.0\""
    ",\"method\":\"%s\""
    ",\"%s\":"
    "{\"difficulty\":%" PRIu64
    ",\"height\":%" PRIu64
    ",\"job_id\":%" PRIu32
    ",\"pre_pow\":\"%s\""
    "}}\n",
    idStr.empty() ? "\"Stratum\"" : idStr.c_str(),
    idStr.empty() ? "job" : "getjobtemplate",
    idStr.empty() ? "params" : "result",
    currentDifficulty_,
    job->height_,
    prePowHash,
    job->prePowStr_.c_str());

  DLOG(INFO) << strNotify;
  sendData(strNotify); // send notify string

  clearLocalJobs();
}