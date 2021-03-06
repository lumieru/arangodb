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

#include "ClusterTraverser.h"
#include "Basics/VelocyPackHelper.h"
#include "Cluster/ClusterMethods.h"

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

using namespace arangodb;

using ClusterTraverser = arangodb::traverser::ClusterTraverser;

bool ClusterTraverser::VertexGetter::getVertex(std::string const& edgeId,
                                               std::string const& vertexId,
                                               size_t depth,
                                               std::string& result) {

  auto it = _traverser->_edges.find(edgeId);
  if (it != _traverser->_edges.end()) {
    VPackSlice slice(it->second->data());
    std::string from = slice.get(StaticStrings::FromString).copyString();
    if (from != vertexId) {
      result = std::move(from);
    } else {
      std::string to = slice.get(StaticStrings::ToString).copyString();
      result = std::move(to);
    }
    auto exp = _traverser->_opts.expressions->find(depth);
    if (exp != _traverser->_opts.expressions->end()) {
      auto v = _traverser->_vertices.find(result);
      if (v == _traverser->_vertices.end()) {
        // If the vertex ist not in list it means it has not passed any
        // filtering up to now
        ++_traverser->_filteredPaths;
        result = "";
        return false;
      }
      if (!_traverser->vertexMatchesCondition(VPackSlice(v->second->data()), exp->second)) {
        result = "";
        return false;
      }
    }
    return true;
  }
  // This should never be reached
  result = "";
  return false;
}

void ClusterTraverser::VertexGetter::reset() {
  // Nothing to do here. Subclass has to clear list of already returned vertices.
}

bool ClusterTraverser::UniqueVertexGetter::getVertex(
    std::string const& edgeId, std::string const& vertexId, size_t depth,
    std::string& result) {

  auto it = _traverser->_edges.find(edgeId);
  if (it != _traverser->_edges.end()) {
    VPackSlice slice(it->second->data());
    std::string from = slice.get(StaticStrings::FromString).copyString();
    if (from != vertexId) {
      result = std::move(from);
    } else {
      std::string to = slice.get(StaticStrings::ToString).copyString();
      result = std::move(to);
    }
    
    if (_returnedVertices.find(result) != _returnedVertices.end()) {
      // This vertex is not unique.
      ++_traverser->_filteredPaths;
      result = "";
      return false;
    }

    auto exp = _traverser->_opts.expressions->find(depth);
    if (exp != _traverser->_opts.expressions->end()) {
      auto v = _traverser->_vertices.find(result);
      if (v == _traverser->_vertices.end()) {
        // If the vertex ist not in list it means it has not passed any
        // filtering up to now
        ++_traverser->_filteredPaths;
        result = "";
        return false;
      }
      if (!_traverser->vertexMatchesCondition(VPackSlice(v->second->data()), exp->second)) {
        result = "";
        return false;
      }
    }
    _returnedVertices.emplace(result);
    return true;
  }
  // This should never be reached
  result = "";
  return false;
}

void ClusterTraverser::UniqueVertexGetter::reset() {
  _returnedVertices.clear();
}

void ClusterTraverser::ClusterEdgeGetter::getEdge(
    std::string const& startVertex, std::vector<std::string>& result,
    size_t*& last, size_t& eColIdx) {
  std::string collName;
  TRI_edge_direction_e dir;
  if (!_traverser->_opts.getCollection(eColIdx, collName, dir)) {
    // Nothing to do, caller has set a defined state already.
    return;
  }
  if (last == nullptr) {
    size_t depth = result.size();
    TRI_ASSERT(_traverser->_iteratorCache.size() == result.size());
    // We have to request the next level
    arangodb::GeneralResponse::ResponseCode responseCode;
    std::vector<TraverserExpression*> expEdges;
    auto found = _traverser->_opts.expressions->find(depth);
    if (found != _traverser->_opts.expressions->end()) {
      expEdges = found->second;
    }

    VPackBuilder resultEdges;
    resultEdges.openObject();
    int res = getFilteredEdgesOnCoordinator(
        _traverser->_dbname, collName, startVertex, dir,
        expEdges, responseCode, resultEdges);
    if (res != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION(res);
    }
    resultEdges.close();
    VPackSlice resSlice = resultEdges.slice();
    VPackSlice edgesSlice = resSlice.get("edges");
    VPackSlice statsSlice = resSlice.get("stats");

    size_t read = arangodb::basics::VelocyPackHelper::getNumericValue<size_t>(
        statsSlice, "scannedIndex", 0);
    size_t filter = arangodb::basics::VelocyPackHelper::getNumericValue<size_t>(
        statsSlice, "filtered", 0);
    _traverser->_readDocuments += read;
    _traverser->_filteredPaths += filter;

    if (edgesSlice.isNone() || edgesSlice.length() == 0) {
      last = nullptr;
      eColIdx++;
      getEdge(startVertex, result, last, eColIdx);
      return;
    }
    std::stack<std::string> stack;
    std::unordered_set<std::string> verticesToFetch;
    for (auto const& edge : VPackArrayIterator(edgesSlice)) {
      std::string edgeId = arangodb::basics::VelocyPackHelper::getStringValue(
          edge, StaticStrings::IdString.c_str(), "");
      if (_traverser->_opts.uniqueEdges == TraverserOptions::UniquenessLevel::GLOBAL) {
        // DO not push this edge on the stack.
        if (_traverser->_edges.find(edgeId) != _traverser->_edges.end()) {
          continue;
        }
      }
      std::string fromId = arangodb::basics::VelocyPackHelper::getStringValue(
          edge, StaticStrings::FromString.c_str(), "");
      if (_traverser->_vertices.find(fromId) == _traverser->_vertices.end()) {
        verticesToFetch.emplace(std::move(fromId));
      }
      std::string toId = arangodb::basics::VelocyPackHelper::getStringValue(
          edge, StaticStrings::ToString.c_str(), "");
      if (_traverser->_vertices.find(toId) == _traverser->_vertices.end()) {
        verticesToFetch.emplace(std::move(toId));
      }
      VPackBuilder tmpBuilder;
      tmpBuilder.add(edge);
      _traverser->_edges.emplace(edgeId, tmpBuilder.steal());
      stack.push(std::move(edgeId));
    }

    if (stack.empty()) {
      // We did not find any valid edge here.
      // Try next index
      last = nullptr;
      eColIdx++;
      getEdge(startVertex, result, last, eColIdx);
      return;
    }

    _traverser->fetchVertices(verticesToFetch, depth + 1);

    std::string next = stack.top();
    stack.pop();
    last = &_continueConst;
    _traverser->_iteratorCache.emplace(stack);
    if (_traverser->_opts.uniqueEdges == TraverserOptions::UniquenessLevel::PATH) {
      auto search = std::find(result.begin(), result.end(), next);
      if (search != result.end()) {
        // The edge is now included twice. Go on with the next
        getEdge(startVertex, result, last, eColIdx);
        return;
      }
    }
    result.push_back(std::move(next));
  } else {
    if (_traverser->_iteratorCache.empty()) {
      last = nullptr;
      return;
    }
    std::stack<std::string>& tmp = _traverser->_iteratorCache.top();
    if (tmp.empty()) {
      _traverser->_iteratorCache.pop();
      last = nullptr;
      eColIdx++;
      getEdge(startVertex, result, last, eColIdx);
      return;
    } else {
      std::string next = tmp.top();
      tmp.pop();
      if (_traverser->_opts.uniqueEdges == TraverserOptions::UniquenessLevel::PATH) {
        auto search = std::find(result.begin(), result.end(), next);
        if (search != result.end()) {
          // The edge would be included twice. Go on with the next
          getEdge(startVertex, result, last, eColIdx);
          return;
        }
      }
      result.push_back(std::move(next));
    }
  }
}

void ClusterTraverser::ClusterEdgeGetter::getAllEdges(
    std::string const& startVertex, std::unordered_set<std::string>& result,
    size_t depth) {
  std::string collName;
  TRI_edge_direction_e dir;
  size_t eColIdx = 0;
  std::vector<TraverserExpression*> expEdges;
  auto found = _traverser->_opts.expressions->find(depth);
  if (found != _traverser->_opts.expressions->end()) {
    expEdges = found->second;
  }

  arangodb::GeneralResponse::ResponseCode responseCode;
  VPackBuilder resultEdges;
  std::unordered_set<std::string> verticesToFetch;
  while (_traverser->_opts.getCollection(eColIdx++, collName, dir)) {
    resultEdges.clear();
    resultEdges.openObject();
    int res = getFilteredEdgesOnCoordinator(
        _traverser->_dbname, collName, startVertex, dir,
        expEdges, responseCode, resultEdges);
    if (res != TRI_ERROR_NO_ERROR) {
      THROW_ARANGO_EXCEPTION(res);
    }
    resultEdges.close();
    VPackSlice resSlice = resultEdges.slice();
    VPackSlice edgesSlice = resSlice.get("edges");
    VPackSlice statsSlice = resSlice.get("stats");

    size_t read = arangodb::basics::VelocyPackHelper::getNumericValue<size_t>(
        statsSlice, "scannedIndex", 0);
    size_t filter = arangodb::basics::VelocyPackHelper::getNumericValue<size_t>(
        statsSlice, "filtered", 0);
    _traverser->_readDocuments += read;
    _traverser->_filteredPaths += filter;
    if (edgesSlice.isNone() || edgesSlice.length() == 0) {
      // No edges found here
      continue;
    }
    for (auto const& edge : VPackArrayIterator(edgesSlice)) {
      std::string edgeId = arangodb::basics::VelocyPackHelper::getStringValue(
          edge, StaticStrings::IdString.c_str(), "");
      if (_traverser->_opts.uniqueEdges ==
          TraverserOptions::UniquenessLevel::GLOBAL) {
        // DO not push this edge on the stack.
        if (_traverser->_edges.find(edgeId) != _traverser->_edges.end()) {
          continue;
        }
      }
      std::string fromId = arangodb::basics::VelocyPackHelper::getStringValue(
          edge, StaticStrings::FromString.c_str(), "");
      if (_traverser->_vertices.find(fromId) == _traverser->_vertices.end()) {
        verticesToFetch.emplace(std::move(fromId));
      }
      std::string toId = arangodb::basics::VelocyPackHelper::getStringValue(
          edge, StaticStrings::ToString.c_str(), "");
      if (_traverser->_vertices.find(toId) == _traverser->_vertices.end()) {
        verticesToFetch.emplace(std::move(toId));
      }
      VPackBuilder tmpBuilder;
      tmpBuilder.add(edge);
      _traverser->_edges.emplace(edgeId, tmpBuilder.steal());
      result.emplace(std::move(edgeId));
    }
  }
  _traverser->fetchVertices(verticesToFetch, depth + 1);
}

void ClusterTraverser::setStartVertex(std::string const& id) {
  _vertexGetter->reset();
  if (_opts.useBreadthFirst) {
    _enumerator.reset(
        new arangodb::traverser::BreadthFirstEnumerator(this, id, &_opts));
    _vertexGetter->setStartVertex(id);
  } else {
    _enumerator.reset(
        new arangodb::traverser::DepthFirstEnumerator(this, id, &_opts));
  }
  _done = false;
  auto it = _vertices.find(id);
  if (it == _vertices.end()) {
    size_t firstSlash = id.find("/");
    if (firstSlash == std::string::npos ||
        id.find("/", firstSlash + 1) != std::string::npos) {
      // We can stop here. The start vertex is not a valid _id
      ++_filteredPaths;
      _done = true;
      return;
    }
    std::unordered_set<std::string> vertexToFetch;
    vertexToFetch.emplace(id);
    fetchVertices(vertexToFetch, 0); // this inserts the vertex
    it = _vertices.find(id);
    if (it == _vertices.end()) {
      // We can stop here. The start vertex does not match condition.
      ++_filteredPaths;
      _done = true;
      return;
    }
  }

  auto exp = _opts.expressions->find(0);
  if (exp != _opts.expressions->end() &&
      !vertexMatchesCondition(VPackSlice(it->second->data()), exp->second)) {
    // We can stop here. The start vertex does not match condition
    _done = true;
  }
}

void ClusterTraverser::getEdge(std::string const& startVertex,
                               std::vector<std::string>& result, size_t*& last,
                               size_t& eColIdx) {
  return _edgeGetter->getEdge(startVertex, result, last, eColIdx);
}

void ClusterTraverser::getAllEdges(
    std::string const& startVertex, std::unordered_set<std::string>& result,
    size_t depth) {
  return _edgeGetter->getAllEdges(startVertex, result, depth);
}

bool ClusterTraverser::getVertex(std::string const& edgeId,
                                 std::string const& vertexId, size_t depth,
                                 std::string& result) {
  return _vertexGetter->getVertex(edgeId, vertexId, depth, result);
}

void ClusterTraverser::fetchVertices(std::unordered_set<std::string>& verticesToFetch, size_t depth) {
  _readDocuments += verticesToFetch.size();

  std::vector<TraverserExpression*> expVertices;
  auto found = _opts.expressions->find(depth);
  if (found != _opts.expressions->end()) {
    expVertices = found->second;
  }

  int res = getFilteredDocumentsOnCoordinator(_dbname, expVertices,
                                              verticesToFetch, _vertices);
  if (res != TRI_ERROR_NO_ERROR && 
      res != TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND) {
    THROW_ARANGO_EXCEPTION(res);
  }

  // By convention verticesToFetch now contains all _ids of vertices that
  // could not be found.
  // Store them as NULL
  for (auto const& it : verticesToFetch) {
    VPackBuilder builder;
    builder.add(VPackValue(VPackValueType::Null));
    _vertices.emplace(it, builder.steal());
  }
}

bool ClusterTraverser::vertexMatchesCondition(
    VPackSlice const& v,
    std::vector<arangodb::traverser::TraverserExpression*> const& exp) {
  for (auto const& e : exp) {
    if (!e->isEdgeAccess) {
      if (v.isNone() || !e->matchesCheck(_trx, v)) {
        ++_filteredPaths;
        return false;
      }
    }
  }
  return true;
}

bool ClusterTraverser::next() {
  TRI_ASSERT(!_done);
  if (_pruneNext) {
    _pruneNext = false;
    _enumerator->prune();
  }
  TRI_ASSERT(!_pruneNext);
  return _enumerator->next();
  /*
  if (_opts.useBreadthFirst &&
      _opts.uniqueVertices == TraverserOptions::UniquenessLevel::NONE &&
      _opts.uniqueEdges == TraverserOptions::UniquenessLevel::PATH) {
    // Only if we use breadth first
    // and vertex uniqueness is not guaranteed
    // We have to validate edges on path uniqueness.
    // Otherwise this situation cannot occur.
    // If two edges are identical than at least their start or end vertex
    // is on the path twice: A -> B <- A
    for (size_t i = 0; i < countEdges; ++i) {
      for (size_t j = i + 1; j < countEdges; ++j) {
        if (path.edges[i] == path.edges[j]) {
          // We found two idential edges. Prune.
          // Next
          _pruneNext = true;
          return next();
        }
      }
    }
  }
  */
}

aql::AqlValue ClusterTraverser::fetchVertexData(std::string const& id) {
  auto cached = _vertices.find(id);
  // All vertices are cached!!
  TRI_ASSERT(cached != _vertices.end());
  return aql::AqlValue((*cached).second->data());
}

aql::AqlValue ClusterTraverser::fetchEdgeData(std::string const& id) {
  auto cached = _edges.find(id);
  // All edges are cached!!
  TRI_ASSERT(cached != _edges.end());
  return aql::AqlValue((*cached).second->data());
}

//////////////////////////////////////////////////////////////////////////////
/// @brief Function to add the real data of a vertex into a velocypack builder
//////////////////////////////////////////////////////////////////////////////

void ClusterTraverser::addVertexToVelocyPack(std::string const& id,
                           arangodb::velocypack::Builder& result) {
  auto cached = _vertices.find(id);
  // All vertices are cached!!
  TRI_ASSERT(cached != _vertices.end());
  result.add(VPackSlice((*cached).second->data()));
}

//////////////////////////////////////////////////////////////////////////////
/// @brief Function to add the real data of an edge into a velocypack builder
//////////////////////////////////////////////////////////////////////////////

void ClusterTraverser::addEdgeToVelocyPack(std::string const& id,
                         arangodb::velocypack::Builder& result) {
  auto cached = _edges.find(id);
  // All edges are cached!!
  TRI_ASSERT(cached != _edges.end());
  result.add(VPackSlice((*cached).second->data()));
}

aql::AqlValue ClusterTraverser::lastVertexToAqlValue() {
  return _enumerator->lastVertexToAqlValue();
}

aql::AqlValue ClusterTraverser::lastEdgeToAqlValue() {
  return _enumerator->lastEdgeToAqlValue();
}

aql::AqlValue ClusterTraverser::pathToAqlValue(VPackBuilder& builder) {
  return _enumerator->pathToAqlValue(builder);
}
