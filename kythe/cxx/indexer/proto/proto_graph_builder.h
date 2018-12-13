/*
 * Copyright 2018 The Kythe Authors. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Wrappers for adding nodes/edges to the Kythe graph.

#ifndef KYTHE_CXX_INDEXER_PROTO_PROTO_GRAPH_BUILDER_H_
#define KYTHE_CXX_INDEXER_PROTO_PROTO_GRAPH_BUILDER_H_

#include <string>
#include <utility>
#include <vector>

#include "glog/logging.h"
#include "google/protobuf/io/printer.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "kythe/cxx/common/indexing/KytheGraphRecorder.h"
#include "kythe/cxx/common/indexing/KytheOutputStream.h"
#include "kythe/cxx/common/kythe_uri.h"
#include "kythe/cxx/common/protobuf_metadata_file.h"
#include "kythe/proto/common.pb.h"
#include "kythe/proto/storage.pb.h"
#include "kythe/proto/xref.pb.h"

namespace kythe {

// The canonical name for the protobuf language in Kythe
static const char kLanguageName[] = "protobuf";

// A simple structure for the offsets we need to make anchor nodes, because
// Descriptors omit these and kythe::proto::Location is seriously overkill.
struct Location {
  proto::VName file;
  size_t begin;
  size_t end;
};

// Contains the hooks for emitting the correct Kythe artifacts based at
// relevant points in a proto file's source tree.
class ProtoGraphBuilder {
 public:
  // Constructs a graph builder using the given graph recorder. Ownership
  // of the graph recorder is not transferred, and it must outlive this
  // object. `vname_for_rel_path` should return a VName for the given
  // relative (to the file under analysis) path.
  ProtoGraphBuilder(
      KytheGraphRecorder* recorder,
      std::function<proto::VName(const std::string&)> vname_for_rel_path)
      : recorder_(recorder),
        vname_for_rel_path_(std::move(vname_for_rel_path)) {}

  // disallow copy and assign
  ProtoGraphBuilder(const ProtoGraphBuilder&) = delete;
  void operator=(const ProtoGraphBuilder&) = delete;

  // Returns a VName for the given protobuf descriptor. Descriptors share
  // various member names but do not participate in any sort of inheritance
  // hierarchy, so we're stuck with a template.
  template <typename SomeDescriptor>
  proto::VName VNameForDescriptor(const SomeDescriptor* descriptor) {
    proto::VName vname;
    class PathSink : public ::google::protobuf::io::AnnotationCollector {
     public:
      PathSink(const std::function<proto::VName(const std::string&)>&
                   vname_for_rel_path,
               proto::VName* vname)
          : vname_for_rel_path_(vname_for_rel_path), vname_(vname) {}

      void AddAnnotation(size_t begin_offset, size_t end_offset,
                         const std::string& file_path,
                         const std::vector<int>& path) override {
        *vname_ = VNameForProtoPath(vname_for_rel_path_(file_path), path);
      }

     private:
      const std::function<proto::VName(const std::string&)>&
          vname_for_rel_path_;
      proto::VName* vname_;
    } path_sink(vname_for_rel_path_, &vname);
    // We'd really like to use GetLocationPath here, but it's private, so
    // we have to go through some contortions. On the plus side, this is the
    // *exact* same code that protoc backends use for writing out annotations,
    // so if AddAnnotation ever changes we'll know.
    std::string s;
    ::google::protobuf::io::StringOutputStream stream(&s);
    ::google::protobuf::io::Printer printer(&stream, '$', &path_sink);
    printer.Print("$0$", "0", "0");
    printer.Annotate("0", descriptor);
    return vname;
  }

  // Sets the source text for this file.
  void SetText(const proto::VName& node_name, const std::string& content);

  // Records a node with the given VName and kind in the graph.
  void AddNode(const proto::VName& node_name, NodeKindID node_kind);

  // Records an edge of the given kind between the named nodes in the graph.
  void AddEdge(const proto::VName& start, const proto::VName& end,
               EdgeKindID start_to_end_kind);

  // Creates and add to the graph a proto language-specific declaration node.
  proto::VName CreateAndAddAnchorNode(const Location& location);

  // Creates and adds a documentation node for `element` to the graph. The
  // `location` is used to derive the location of the documentation text.
  proto::VName CreateAndAddDocNode(const Location& location,
                                   const proto::VName& element);

  // Adds an import for the file.
  void AddImport(const std::string& import, const Location& location);

  // Adds a namespace for the file.  Generally the first call.
  void AddNamespace(const proto::VName& ticket, const Location& location);

  // Adds a value field to an already-added enum declaration.
  void AddValueToEnum(const proto::VName& enum_type, const proto::VName& value,
                      const Location& location);

  // Adds a field to an already-added protocol buffer message.
  // `parent` refers to the context where the field is defined,
  // and `message` refers to the message this field is a part of.
  // These only differ when processing extensions.
  void AddFieldToMessage(const proto::VName* parent,
                         const proto::VName& message, const proto::VName* oneof,
                         const proto::VName& field, const Location& location);

  // Adds a oneof to an already-added protocol buffer message.
  void AddOneofToMessage(const proto::VName& message, const proto::VName& oneof,
                         const Location& location);

  // Adds a stubby method to an already-added RPC service.
  void AddMethodToService(const proto::VName& service,
                          const proto::VName& method, const Location& location);

  // Adds an enum.
  void AddEnumType(const proto::VName* parent, const proto::VName& enum_type,
                   const Location& location);

  // Adds a message.
  void AddMessageType(const proto::VName* parent, const proto::VName& message,
                      const Location& location);

  // Adds an argument with type given by type at the specified location to an
  // already-added stubby service method.
  void AddArgumentToMethod(const proto::VName& method, const proto::VName& type,
                           const Location& location) {
    AddReference(type, location);
  }

  // Adds an anchor for location and a Ref edge to referent
  void AddReference(const proto::VName& referent, const Location& location);

  // Adds an edge indicating that `term` has type `type`.
  void AddTyping(const proto::VName& term, const proto::VName& type);

  // Adds a stubby service to the file or optionally provided namespace scope.
  // Nested fields/declarations/etc must be added separately.
  void AddService(const proto::VName* parent, const proto::VName& service,
                  const Location& location);

  // Adds an edge associating the comment at a location with an element.
  void AddDocComment(const proto::VName& element, const Location& location);

  // Adds a code fact to the element.
  void AddCodeFact(const proto::VName& element, const MarkedSource& code);

 private:
  // Where we output nodes, edges, etc..
  KytheGraphRecorder* recorder_;

  // A function to resolve relative paths to VNames.
  std::function<proto::VName(const std::string&)> vname_for_rel_path_;

  // The text of the current file being analyzed.
  std::string current_file_contents_;
};

}  // namespace kythe

#endif  // KYTHE_CXX_INDEXER_PROTO_PROTO_GRAPH_BUILDER_H_
