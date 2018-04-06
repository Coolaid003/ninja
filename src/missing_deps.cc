// Copyright 2019 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "missing_deps.h"

#include <string.h>

#include <iostream>

#include "depfile_parser.h"
#include "deps_log.h"
#include "disk_interface.h"
#include "graph.h"
#include "state.h"
#include "util.h"

namespace {

/// ImplicitDepLoader variant that stores dep nodes into the given output
/// without updating graph deps like the base loader does.
struct NodeStoringImplicitDepLoader : public ImplicitDepLoader {
  NodeStoringImplicitDepLoader(
      State* state, DepsLog* deps_log, DiskInterface* disk_interface,
      DepfileParserOptions const* depfile_parser_options,
      std::vector<Node*>* dep_nodes_output)
      : ImplicitDepLoader(state, deps_log, disk_interface,
                          depfile_parser_options),
        dep_nodes_output_(dep_nodes_output) {}

 protected:
  virtual bool ProcessDepfileDeps(Edge* edge,
                                  std::vector<StringPiece>* depfile_ins,
                                  std::string* err);

 private:
  std::vector<Node*>* dep_nodes_output_;
};

bool NodeStoringImplicitDepLoader::ProcessDepfileDeps(
    Edge* edge, std::vector<StringPiece>* depfile_ins, std::string* err) {
  for (auto & item : *depfile_ins) {
    uint64_t slash_bits;
    CanonicalizePath(const_cast<char*>(item.str_), &item.len_, &slash_bits);
    Node* node = state_->GetNode(item, slash_bits);
    dep_nodes_output_->push_back(node);
  }
  return true;
}

}  // namespace

MissingDependencyScannerDelegate::~MissingDependencyScannerDelegate() {}

void MissingDependencyPrinter::OnMissingDep(Node* node, const std::string& path,
                                            const Rule& generator) {
  std::cout << "Missing dep: " << node->path() << " uses " << path
            << " (generated by " << generator.name() << ")\n";
}

MissingDependencyScanner::MissingDependencyScanner(
    MissingDependencyScannerDelegate* delegate, DepsLog* deps_log, State* state,
    DiskInterface* disk_interface)
    : delegate_(delegate), deps_log_(deps_log), state_(state),
      disk_interface_(disk_interface), missing_dep_path_count_(0) {}

void MissingDependencyScanner::ProcessNode(Node* node) {
  if (!node)
    return;
  Edge* edge = node->in_edge();
  if (!edge)
    return;
  if (!seen_.insert(node).second)
    return;

  for (auto const& in : edge->inputs_) {
    ProcessNode(in);
  }

  std::string deps_type = edge->GetBinding("deps");
  if (!deps_type.empty()) {
    DepsLog::Deps* deps = deps_log_->GetDeps(node);
    if (deps)
      ProcessNodeDeps(node, deps->nodes, deps->node_count);
  } else {
    DepfileParserOptions parser_opts;
    std::vector<Node*> depfile_deps;
    NodeStoringImplicitDepLoader dep_loader(state_, deps_log_, disk_interface_,
                                            &parser_opts, &depfile_deps);
    std::string err;
    dep_loader.LoadDeps(edge, &err);
    if (!depfile_deps.empty())
      ProcessNodeDeps(node, &depfile_deps[0], depfile_deps.size());
  }
}

void MissingDependencyScanner::ProcessNodeDeps(Node* node, Node** dep_nodes,
                                               int dep_nodes_count) {
  Edge* edge = node->in_edge();
  std::set<Edge*> deplog_edges;
  for (int i = 0; i < dep_nodes_count; ++i) {
    Node* deplog_node = dep_nodes[i];
    // Special exception: A dep on build.ninja can be used to mean "always
    // rebuild this target when the build is reconfigured", but build.ninja is
    // often generated by a configuration tool like cmake or gn. The rest of
    // the build "implicitly" depends on the entire build being reconfigured,
    // so a missing dep path to build.ninja is not an actual missing dependency
    // problem.
    if (deplog_node->path() == "build.ninja")
      return;
    Edge* deplog_edge = deplog_node->in_edge();
    if (deplog_edge) {
      deplog_edges.insert(deplog_edge);
    }
  }
  std::vector<Edge*> missing_deps;
  for (auto const& de : deplog_edges) {
    if (!PathExistsBetween(de, edge)) {
      missing_deps.push_back(de);
    }
  }

  if (!missing_deps.empty()) {
    std::set<std::string> missing_deps_rule_names;
    for (auto const& item : missing_deps) {
      for (int i = 0; i < dep_nodes_count; ++i) {
        if (dep_nodes[i]->in_edge() == item) {
          generated_nodes_.insert(dep_nodes[i]);
          generator_rules_.insert(&item->rule());
          missing_deps_rule_names.insert(item->rule().name());
          delegate_->OnMissingDep(node, dep_nodes[i]->path(), item->rule());
        }
      }
    }
    missing_dep_path_count_ += missing_deps_rule_names.size();
    nodes_missing_deps_.insert(node);
  }
}

void MissingDependencyScanner::PrintStats() {
  std::cout << "Processed " << seen_.size() << " nodes.\n";
  if (HadMissingDeps()) {
    std::cout << "Error: There are " << missing_dep_path_count_
              << " missing dependency paths.\n";
    std::cout << nodes_missing_deps_.size()
              << " targets had depfile dependencies on "
              << generated_nodes_.size() << " distinct generated inputs "
              << "(from " << generator_rules_.size() << " rules) "
              << " without a non-depfile dep path to the generator.\n";
    std::cout << "There might be build flakiness if any of the targets listed "
                 "above are built alone, or not late enough, in a clean output "
                 "directory.\n";
  } else {
    std::cout << "No missing dependencies on generated files found.\n";
  }
}

bool MissingDependencyScanner::PathExistsBetween(Edge* from, Edge* to) {
  AdjacencyMap::iterator it = adjacency_map_.find(from);
  if (it != adjacency_map_.end()) {
    InnerAdjacencyMap::iterator inner_it = it->second.find(to);
    if (inner_it != it->second.end()) {
      return inner_it->second;
    }
  } else {
    it = adjacency_map_.insert(std::make_pair(from, InnerAdjacencyMap())).first;
  }
  bool found = false;
  for (auto const& input : to->inputs_) {
    Edge* e = input->in_edge();
    if (e && (e == from || PathExistsBetween(from, e))) {
      found = true;
      break;
    }
  }
  it->second.insert(std::make_pair(to, found));
  return found;
}
