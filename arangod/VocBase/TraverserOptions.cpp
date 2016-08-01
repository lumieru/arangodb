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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "TraverserOptions.h"

#include "Aql/Ast.h"
#include "Aql/Expression.h"
#include "Aql/Query.h"
#include "Basics/JsonHelper.h"
#include "VocBase/SingleServerTraverser.h"

using Json = arangodb::basics::Json;
using JsonHelper = arangodb::basics::JsonHelper;

arangodb::traverser::TraverserOptions::LookupInfo::LookupInfo()
    : expression(nullptr), indexCondition(nullptr) {
  // NOTE: We need exactly one in this case for the optimizer to update
  idxHandles.resize(1);
};

arangodb::traverser::TraverserOptions::LookupInfo::~LookupInfo() {
  if (expression != nullptr) {
    delete expression;
  }
}

arangodb::traverser::TraverserOptions::LookupInfo::LookupInfo(
    arangodb::aql::Query* query, VPackSlice const& info, VPackSlice const& shards) {
  TRI_ASSERT(shards.isArray());
  idxHandles.reserve(shards.length());

  VPackSlice read = info.get("handle");
  if (!read.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "Each lookup requires handle to be an object");
  }

  read = read.get("id");
  if (!read.isString()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "Each handle requires id to be a string");
  }
  std::string idxId = read.copyString();
  auto trx = query->trx();

  for (auto const& it : VPackArrayIterator(shards)) {
    if (!it.isString()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_BAD_PARAMETER,
          "Shards have to be a list of strings");
    }
    idxHandles.emplace_back(trx->getIndexByIdentifier(it.copyString(), idxId));
  }

  read = info.get("expression");
  if (!read.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "Each lookup requires expression to be an object");
  }
  arangodb::basics::Json expJson(TRI_UNKNOWN_MEM_ZONE,
      arangodb::basics::VelocyPackHelper::velocyPackToJson(read));
  expression = new aql::Expression(query->ast(), expJson);

  read = info.get("condition");
  if (!read.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "Each lookup requires condition to be an object");
  }
  arangodb::basics::Json condJson(TRI_UNKNOWN_MEM_ZONE,
      arangodb::basics::VelocyPackHelper::velocyPackToJson(read));
  indexCondition = new aql::AstNode(query->ast(), condJson); 
}

arangodb::traverser::TraverserOptions::LookupInfo::LookupInfo(LookupInfo const& other) {
  idxHandles = other.idxHandles;
  expression = other.expression->clone();
  indexCondition = other.indexCondition;
}

void arangodb::traverser::TraverserOptions::LookupInfo::buildEngineInfo(
    VPackBuilder& result) const {
  result.openObject();
  result.add(VPackValue("handle"));
  // We only run toVelocyPack on Coordinator.
  TRI_ASSERT(idxHandles.size() == 1);
  result.openObject();
  idxHandles[0].toVelocyPack(result, false);
  result.close();
  result.add(VPackValue("expression"));
  expression->toVelocyPack(result, false);
  result.add(VPackValue("condition"));
  indexCondition->toVelocyPack(result, false);
  result.close();
}

arangodb::traverser::TraverserOptions::TraverserOptions(
    arangodb::Transaction* trx, Json const& json)
    : _trx(trx),
      _tmpVar(nullptr),
      _ctx(new aql::FixedVarExpressionContext()),
      minDepth(1),
      maxDepth(1),
      useBreadthFirst(false),
      uniqueVertices(UniquenessLevel::NONE),
      uniqueEdges(UniquenessLevel::PATH) {
  Json obj = json.get("traversalFlags");

  minDepth = JsonHelper::getNumericValue<uint64_t>(obj.json(), "minDepth", 1);
  maxDepth = JsonHelper::getNumericValue<uint64_t>(obj.json(), "maxDepth", 1);
  TRI_ASSERT(minDepth <= maxDepth);
  useBreadthFirst = JsonHelper::getBooleanValue(obj.json(), "bfs", false);
  std::string tmp = JsonHelper::getStringValue(obj.json(), "uniqueVertices", "");
  if (tmp == "path") {
    uniqueVertices =
        arangodb::traverser::TraverserOptions::UniquenessLevel::PATH;
  } else if (tmp == "global") {
    uniqueVertices =
        arangodb::traverser::TraverserOptions::UniquenessLevel::GLOBAL;
  } else {
    uniqueVertices =
        arangodb::traverser::TraverserOptions::UniquenessLevel::NONE;
  }

  tmp = JsonHelper::getStringValue(obj.json(), "uniqueEdges", "");
  if (tmp == "none") {
    uniqueEdges =
        arangodb::traverser::TraverserOptions::UniquenessLevel::NONE;
  } else if (tmp == "global") {
    uniqueEdges =
        arangodb::traverser::TraverserOptions::UniquenessLevel::GLOBAL;
  } else {
    uniqueEdges =
        arangodb::traverser::TraverserOptions::UniquenessLevel::PATH;
  }
}

arangodb::traverser::TraverserOptions::TraverserOptions(
    arangodb::aql::Query* query, VPackSlice info, VPackSlice collections)
    : _trx(query->trx()),
      _tmpVar(nullptr),
      _ctx(new aql::FixedVarExpressionContext()),
      minDepth(1),
      maxDepth(1),
      useBreadthFirst(false),
      uniqueVertices(UniquenessLevel::NONE),
      uniqueEdges(UniquenessLevel::PATH) {
      // NOTE collections is an array of arrays of strings
  VPackSlice read = info.get("minDepth");
  if (!read.isInteger()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The options require a minDepth");
  }
  minDepth = read.getNumber<uint64_t>();

  read = info.get("maxDepth");
  if (!read.isInteger()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The options require a maxDepth");
  }
  maxDepth = read.getNumber<uint64_t>();

  read = info.get("bfs");
  if (!read.isBoolean()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The options require a bfs");
  }
  useBreadthFirst = read.getBool();

  read = info.get("uniqueVertices");
  if (!read.isInteger()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The options require a uniqueVertices");
  }
  size_t i = read.getNumber<size_t>();
  switch (i) {
    case 0:
      uniqueVertices = UniquenessLevel::NONE;
      break;
    case 1:
      uniqueVertices = UniquenessLevel::PATH;
      break;
    case 2:
      uniqueVertices = UniquenessLevel::GLOBAL;
      break;
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     "The options require a uniqueVertices");
  }

  read = info.get("uniqueEdges");
  if (!read.isInteger()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The options require a uniqueEdges");
  }
  i = read.getNumber<size_t>();
  switch (i) {
    case 0:
      uniqueEdges = UniquenessLevel::NONE;
      break;
    case 1:
      uniqueEdges = UniquenessLevel::PATH;
      break;
    case 2:
      uniqueEdges = UniquenessLevel::GLOBAL;
      break;
    default:
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                     "The options require a uniqueEdges");
  }

  read = info.get("baseLookupInfos");
  if (!read.isArray()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The options require a baseLookupInfos");
  }

  size_t length = read.length();
  TRI_ASSERT(read.length() == collections.length());
  _baseLookupInfos.reserve(length);
  for (size_t j = 0; j < length; ++j) {
    _baseLookupInfos.emplace_back(query, read.at(j), collections.at(j));
  }

  read = info.get("depthLookupInfo");
  if (!read.isNone()) {
    if (!read.isObject()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_BAD_PARAMETER,
          "The options require depthLookupInfo to be an object");
    }

    _depthLookupInfo.reserve(read.length());
    for (auto const& depth : VPackObjectIterator(read)) {
      auto it = _depthLookupInfo.emplace(depth.key.getNumber<size_t>(), std::vector<LookupInfo>());
      TRI_ASSERT(it.second);
      VPackSlice list = depth.value;
      it.first->second.reserve(length);
      for (size_t j = 0; j < length; ++j) {
        it.first->second.emplace_back(query, list.at(j), collections.at(j));
      }
    }
  }

  read = info.get("vertexExpressions");
  if (!read.isNone()) {
    if (!read.isObject()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_BAD_PARAMETER,
          "The options require vertexExpressions to be an object");
    }

    _vertexExpressions.reserve(read.length());
    for (auto const& info : VPackObjectIterator(read)) {
      arangodb::basics::Json infoJson(TRI_UNKNOWN_MEM_ZONE,
          arangodb::basics::VelocyPackHelper::velocyPackToJson(info.value));
      auto it =
          _vertexExpressions.emplace(info.key.getNumber<size_t>(),
                                   new aql::Expression(query->ast(), infoJson));
      TRI_ASSERT(it.second);
    }
  }

  read = info.get("tmpVar");
  if (!read.isObject()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                   "The options require a tmpVar");
  }
  _tmpVar = query->ast()->variables()->createVariable(read);
}

arangodb::traverser::TraverserOptions::TraverserOptions(
    TraverserOptions const& other)
    : _trx(other._trx),
      _tmpVar(nullptr),
      _ctx(new aql::FixedVarExpressionContext()),
      minDepth(other.minDepth),
      maxDepth(other.maxDepth),
      useBreadthFirst(other.useBreadthFirst),
      uniqueVertices(other.uniqueVertices),
      uniqueEdges(other.uniqueEdges) {
  TRI_ASSERT(other._baseLookupInfos.empty());
  TRI_ASSERT(other._depthLookupInfo.empty());
  TRI_ASSERT(other._vertexExpressions.empty());
  TRI_ASSERT(other._tmpVar == nullptr);
}

arangodb::traverser::TraverserOptions::~TraverserOptions() {
  for (auto& pair : _vertexExpressions) {
    if (pair.second != nullptr) {
      delete pair.second;
    }
  }
  if (_ctx != nullptr) {
    delete _ctx;
  }
}

void arangodb::traverser::TraverserOptions::toVelocyPack(VPackBuilder& builder) const {
  VPackObjectBuilder guard(&builder);

  builder.add("minDepth", VPackValue(minDepth));
  builder.add("maxDepth", VPackValue(maxDepth));
  builder.add("bfs", VPackValue(useBreadthFirst));

  switch (uniqueVertices) {
    case arangodb::traverser::TraverserOptions::UniquenessLevel::NONE:
      builder.add("uniqueVertices", VPackValue("none"));
      break;
    case arangodb::traverser::TraverserOptions::UniquenessLevel::PATH:
      builder.add("uniqueVertices", VPackValue("path"));
      break;
    case arangodb::traverser::TraverserOptions::UniquenessLevel::GLOBAL:
      builder.add("uniqueVertices", VPackValue("global"));
      break;
  }

  switch (uniqueEdges) {
    case arangodb::traverser::TraverserOptions::UniquenessLevel::NONE:
      builder.add("uniqueEdges", VPackValue("none"));
      break;
    case arangodb::traverser::TraverserOptions::UniquenessLevel::PATH:
      builder.add("uniqueEdges", VPackValue("path"));
      break;
    case arangodb::traverser::TraverserOptions::UniquenessLevel::GLOBAL:
      builder.add("uniqueEdges", VPackValue("global"));
      break;
  }

}

void arangodb::traverser::TraverserOptions::buildEngineInfo(VPackBuilder& result) const {
  result.openObject();
  result.add("minDepth", VPackValue(minDepth));
  result.add("maxDepth", VPackValue(maxDepth));
  result.add("bfs", VPackValue(useBreadthFirst));

  result.add(VPackValue("uniqueVertices"));
  switch (uniqueVertices) {
    case UniquenessLevel::NONE:
      result.add(VPackValue(0));
      break;
    case UniquenessLevel::PATH:
      result.add(VPackValue(1));
      break;
    case UniquenessLevel::GLOBAL:
      result.add(VPackValue(2));
      break;
  }

  result.add(VPackValue("uniqueEdges"));
  switch (uniqueEdges) {
    case UniquenessLevel::NONE:
      result.add(VPackValue(0));
      break;
    case UniquenessLevel::PATH:
      result.add(VPackValue(1));
      break;
    case UniquenessLevel::GLOBAL:
      result.add(VPackValue(2));
      break;
  }

  result.add(VPackValue("baseLookupInfos"));
  result.openArray();
  for (auto const& it: _baseLookupInfos) {
    it.buildEngineInfo(result);
  }
  result.close();

  if (!_depthLookupInfo.empty()) {
    result.add(VPackValue("depthLookupInfo"));
    result.openObject();
    for (auto const& pair : _depthLookupInfo) {
      result.add(VPackValue(pair.first));
      result.openArray();
      for (auto const& it : pair.second) {
        it.buildEngineInfo(result);
      }
      result.close();
    }
    result.close();
  }

  if (!_vertexExpressions.empty()) {
    result.add(VPackValue("vertexExpressions"));
    result.openObject();
    for (auto const& pair : _vertexExpressions) {
      result.add(VPackValue(pair.first));
      // Do we need verbosity true here?
      pair.second->toVelocyPack(result, false);
    }
    result.close();
  }

  result.add(VPackValue("tmpVar"));
  _tmpVar->toVelocyPack(result);

  result.close();
}

bool arangodb::traverser::TraverserOptions::vertexHasFilter(
    size_t depth) const {
  return _vertexExpressions.find(depth) != _vertexExpressions.end();
}

bool arangodb::traverser::TraverserOptions::evaluateEdgeExpression(
    arangodb::velocypack::Slice edge, arangodb::velocypack::Slice vertex,
    size_t depth, size_t cursorId) const {
  bool mustDestroy = false;
  auto specific = _depthLookupInfo.find(depth);
  arangodb::aql::Expression* expression = nullptr;

  VPackValueLength vidLength;
  char const* vid = vertex.getString(vidLength);

  if (specific != _depthLookupInfo.end()) {
    TRI_ASSERT(!specific->second.empty());
    TRI_ASSERT(specific->second.size() > cursorId);
    expression = specific->second[cursorId].expression;
  } else {
    TRI_ASSERT(!_baseLookupInfos.empty());
    TRI_ASSERT(_baseLookupInfos.size() > cursorId);
    expression = _baseLookupInfos[cursorId].expression;
  }
  if (expression != nullptr) {
    TRI_ASSERT(!expression->isV8());
    expression->setVariable(_tmpVar, edge);

    // inject _from/_to value
    auto node = expression->nodeForModification();

    TRI_ASSERT(node->numMembers() > 0);
    auto dirCmp = node->getMemberUnchecked(node->numMembers() - 1);
    TRI_ASSERT(dirCmp->type == aql::NODE_TYPE_OPERATOR_BINARY_EQ); 
    TRI_ASSERT(dirCmp->numMembers() == 2);

    auto idNode = dirCmp->getMemberUnchecked(1);
    TRI_ASSERT(idNode->type == aql::NODE_TYPE_VALUE);
    TRI_ASSERT(idNode->isValueType(aql::VALUE_TYPE_STRING));
    idNode->stealComputedValue();
    idNode->setStringValue(vid, vidLength);

    aql::AqlValue res = expression->execute(_trx, _ctx, mustDestroy);
    TRI_ASSERT(res.isBoolean());
    expression->clearVariable(_tmpVar);
    return res.toBoolean();
  }
  return true;
}

bool arangodb::traverser::TraverserOptions::evaluateVertexExpression(
    arangodb::velocypack::Slice vertex, size_t depth) const {
  bool mustDestroy = false;
  auto specific = _vertexExpressions.find(depth);
  if (specific != _vertexExpressions.end()) {
    arangodb::aql::Expression* expression = specific->second;
    TRI_ASSERT(!expression->isV8());
    expression->setVariable(_tmpVar, vertex);
    aql::AqlValue res = expression->execute(_trx, _ctx, mustDestroy);
    TRI_ASSERT(res.isBoolean());
    expression->clearVariable(_tmpVar);
    return res.toBoolean();
  }
  return true;
}

arangodb::traverser::EdgeCursor*
arangodb::traverser::TraverserOptions::nextCursor(VPackSlice vertex,
                                                  size_t depth) const {
  auto specific = _depthLookupInfo.find(depth);
  std::vector<LookupInfo> list;
  if (specific != _depthLookupInfo.end()) {
    list = specific->second;
  } else {
    list = _baseLookupInfos;
  }

#warning NOTE SINGLE SERVER CASE ONLY!
  auto allCursor = std::make_unique<SingleServerEdgeCursor>(list.size());
  auto& opCursors = allCursor->getCursors();
  VPackValueLength vidLength;
  char const* vid = vertex.getString(vidLength);
  for (auto& info : list) {
    auto& node = info.indexCondition;
    TRI_ASSERT(node->numMembers() > 0);
    auto dirCmp = node->getMemberUnchecked(node->numMembers() - 1);
    TRI_ASSERT(dirCmp->type == aql::NODE_TYPE_OPERATOR_BINARY_EQ);
    TRI_ASSERT(dirCmp->numMembers() == 2);

    auto idNode = dirCmp->getMemberUnchecked(1);
    TRI_ASSERT(idNode->type == aql::NODE_TYPE_VALUE);
    TRI_ASSERT(idNode->isValueType(aql::VALUE_TYPE_STRING));
    idNode->setStringValue(vid, vidLength);
    for (auto const& it : info.idxHandles) {
      opCursors.emplace_back(_trx->indexScanForCondition(
          it, node, _tmpVar, UINT64_MAX, 1000, false));
    }
  }
  return allCursor.release();
}

void arangodb::traverser::TraverserOptions::clearVariableValues() {
  _ctx->clearVariableValues();
}

void arangodb::traverser::TraverserOptions::setVariableValue(
    aql::Variable const* var, aql::AqlValue const value) {
  _ctx->setVariableValue(var, value);
}


