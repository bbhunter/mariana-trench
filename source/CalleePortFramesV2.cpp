/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <Show.h>

#include <mariana-trench/Assert.h>
#include <mariana-trench/CalleePortFramesV2.h>
#include <mariana-trench/Features.h>
#include <mariana-trench/Heuristics.h>
#include <mariana-trench/JsonValidation.h>
#include <mariana-trench/Log.h>

namespace marianatrench {

namespace {

void materialize_via_type_of_ports(
    const Method* callee,
    Context& context,
    const Frame& frame,
    const std::vector<const DexType * MT_NULLABLE>& source_register_types,
    std::vector<const Feature*>& via_type_of_features_added,
    FeatureMayAlwaysSet& inferred_features) {
  if (!frame.via_type_of_ports().is_value() ||
      frame.via_type_of_ports().elements().empty()) {
    return;
  }

  // Materialize via_type_of_ports into features and add them to the inferred
  // features
  for (const auto& port : frame.via_type_of_ports().elements()) {
    if (!port.is_argument() ||
        port.parameter_position() >= source_register_types.size()) {
      ERROR(
          1,
          "Invalid port {} provided for via_type_of ports of method {}.{}",
          port,
          callee->get_class()->str(),
          callee->get_name());
      continue;
    }
    const auto* feature = context.features->get_via_type_of_feature(
        source_register_types[port.parameter_position()]);
    via_type_of_features_added.push_back(feature);
    inferred_features.add_always(feature);
  }
}

void materialize_via_value_of_ports(
    const Method* callee,
    Context& context,
    const Frame& frame,
    const std::vector<std::optional<std::string>>& source_constant_arguments,
    FeatureMayAlwaysSet& inferred_features) {
  if (!frame.via_value_of_ports().is_value() ||
      frame.via_value_of_ports().elements().empty()) {
    return;
  }

  // Materialize via_value_of_ports into features and add them to the inferred
  // features
  for (const auto& port : frame.via_value_of_ports().elements()) {
    if (!port.is_argument() ||
        port.parameter_position() >= source_constant_arguments.size()) {
      ERROR(
          1,
          "Invalid port {} provided for via_value_of ports of method {}.{}",
          port,
          callee->get_class()->str(),
          callee->get_name());
      continue;
    }
    const auto* feature = context.features->get_via_value_of_feature(
        source_constant_arguments[port.parameter_position()]);
    inferred_features.add_always(feature);
  }
}

} // namespace

CalleePortFramesV2::CalleePortFramesV2(
    std::initializer_list<TaintConfig> configs)
    : callee_port_(Root(Root::Kind::Leaf)),
      frames_(FramesByKind::bottom()),
      is_result_or_receiver_sinks_(false) {
  for (const auto& config : configs) {
    mt_assert(!config.is_artificial_source());
    add(config);
  }
}

bool CalleePortFramesV2::GroupEqual::operator()(
    const CalleePortFramesV2& left,
    const CalleePortFramesV2& right) const {
  return left.is_result_or_receiver_sinks_ ==
      right.is_result_or_receiver_sinks_ &&
      left.callee_port() == right.callee_port();
}

std::size_t CalleePortFramesV2::GroupHash::operator()(
    const CalleePortFramesV2& frame) const {
  std::size_t seed = 0;
  boost::hash_combine(seed, std::hash<AccessPath>()(frame.callee_port()));
  boost::hash_combine(seed, frame.is_result_or_receiver_sinks_);
  return seed;
}

void CalleePortFramesV2::GroupDifference::operator()(
    CalleePortFramesV2& left,
    const CalleePortFramesV2& right) const {
  left.difference_with(right);
}

void CalleePortFramesV2::add(const TaintConfig& config) {
  mt_assert(!config.is_artificial_source());
  if (is_bottom()) {
    callee_port_ = config.callee_port();
    is_result_or_receiver_sinks_ = config.is_result_or_receiver_sink();
  } else {
    mt_assert(
        callee_port_ == config.callee_port() &&
        is_result_or_receiver_sinks_ == config.is_result_or_receiver_sink());
  }

  output_paths_.join_with(config.output_paths());
  local_positions_.join_with(config.local_positions());
  frames_.update(config.kind(), [&](const Frames& old_frames) {
    auto new_frames = old_frames;
    new_frames.add(Frame(
        config.kind(),
        config.callee_port(),
        config.callee(),
        config.field_callee(),
        config.call_position(),
        config.distance(),
        config.origins(),
        config.field_origins(),
        config.inferred_features(),
        config.locally_inferred_features(),
        config.user_features(),
        config.via_type_of_ports(),
        config.via_value_of_ports(),
        config.canonical_names()));
    return new_frames;
  });
}

bool CalleePortFramesV2::leq(const CalleePortFramesV2& other) const {
  if (is_bottom()) {
    return true;
  } else if (other.is_bottom()) {
    return false;
  }
  mt_assert(has_same_key(other));
  return frames_.leq(other.frames_) && output_paths_.leq(other.output_paths_) &&
      local_positions_.leq(other.local_positions_);
}

bool CalleePortFramesV2::equals(const CalleePortFramesV2& other) const {
  mt_assert(is_bottom() || other.is_bottom() || has_same_key(other));
  return frames_.equals(other.frames_) &&
      output_paths_.equals(other.output_paths_) &&
      local_positions_.equals(other.local_positions_);
}

void CalleePortFramesV2::join_with(const CalleePortFramesV2& other) {
  mt_if_expensive_assert(auto previous = *this);

  if (is_bottom()) {
    callee_port_ = other.callee_port();
    is_result_or_receiver_sinks_ = other.is_result_or_receiver_sinks_;
  }
  mt_assert(other.is_bottom() || has_same_key(other));

  frames_.join_with(other.frames_);
  output_paths_.join_with(other.output_paths_);
  // Approximate the output paths here to avoid storing very large trees during
  // the analysis of a method. These paths will not be read from during the
  // analysis of a method and will be collapsed when the result/receiver sink
  // causes sink/propagation inference. So pre-emptively collapse here for
  // better performance
  output_paths_.collapse_deeper_than(Heuristics::kMaxInputPathDepth);
  output_paths_.limit_leaves(Heuristics::kMaxInputPathLeaves);
  local_positions_.join_with(other.local_positions_);

  mt_expensive_assert(previous.leq(*this) && other.leq(*this));
}

void CalleePortFramesV2::widen_with(const CalleePortFramesV2& other) {
  mt_if_expensive_assert(auto previous = *this);

  if (is_bottom()) {
    callee_port_ = other.callee_port();
    is_result_or_receiver_sinks_ = other.is_result_or_receiver_sinks_;
  }
  mt_assert(other.is_bottom() || has_same_key(other));

  frames_.widen_with(other.frames_);
  output_paths_.widen_with(other.output_paths_);
  local_positions_.widen_with(other.local_positions_);

  mt_expensive_assert(previous.leq(*this) && other.leq(*this));
}

void CalleePortFramesV2::meet_with(const CalleePortFramesV2& other) {
  if (is_bottom()) {
    callee_port_ = other.callee_port();
    is_result_or_receiver_sinks_ = other.is_result_or_receiver_sinks_;
  }
  mt_assert(other.is_bottom() || has_same_key(other));

  frames_.meet_with(other.frames_);
  output_paths_.meet_with(other.output_paths_);
  local_positions_.meet_with(other.local_positions_);
}

void CalleePortFramesV2::narrow_with(const CalleePortFramesV2& other) {
  if (is_bottom()) {
    callee_port_ = other.callee_port();
    is_result_or_receiver_sinks_ = other.is_result_or_receiver_sinks_;
  }
  mt_assert(other.is_bottom() || has_same_key(other));

  frames_.narrow_with(other.frames_);
  output_paths_.narrow_with(other.output_paths_);
  local_positions_.narrow_with(other.local_positions_);
}

void CalleePortFramesV2::difference_with(const CalleePortFramesV2& other) {
  if (is_bottom()) {
    callee_port_ = other.callee_port();
    is_result_or_receiver_sinks_ = other.is_result_or_receiver_sinks_;
  }
  mt_assert(other.is_bottom() || has_same_key(other));

  // Local positions and output paths apply to all frames. If LHS is not leq
  // RHS, then do not apply the difference operator to the frames because every
  // frame on LHS would not be considered leq its RHS frame.
  if (local_positions_.leq(other.local_positions_) &&
      output_paths_.leq(other.output_paths_)) {
    frames_.difference_like_operation(
        other.frames_,
        [](const Frames& frames_left, const Frames& frames_right) {
          auto frames_copy = frames_left;
          frames_copy.difference_with(frames_right);
          return frames_copy;
        });
  }
}

void CalleePortFramesV2::map(const std::function<void(Frame&)>& f) {
  frames_.map([&](const Frames& frames) {
    auto new_frames = frames;
    new_frames.map(f);
    return new_frames;
  });
}

void CalleePortFramesV2::set_origins_if_empty(const MethodSet& origins) {
  map([&](Frame& frame) {
    if (frame.origins().empty()) {
      frame.set_origins(origins);
    }
  });
}

void CalleePortFramesV2::set_field_origins_if_empty_with_field_callee(
    const Field* field) {
  map([&](Frame& frame) {
    if (frame.field_origins().empty()) {
      frame.set_field_origins(FieldSet{field});
    }
    frame.set_field_callee(field);
  });
}

FeatureMayAlwaysSet CalleePortFramesV2::inferred_features() const {
  // TODO(T91357916): Store inferred features in CalleePortFrames rather than
  // Frame.
  auto result = FeatureMayAlwaysSet::bottom();
  for (const auto& [_, frames] : frames_.bindings()) {
    for (const auto& frame : frames) {
      // Inferred features in this class are always locally (intraprocedurally)
      // inferred.
      result.join_with(frame.locally_inferred_features());
    }
  }
  return result;
}

void CalleePortFramesV2::add_inferred_features(
    const FeatureMayAlwaysSet& features) {
  if (features.empty()) {
    return;
  }

  map([&features](Frame& frame) { frame.add_inferred_features(features); });
}

void CalleePortFramesV2::add_local_position(const Position* position) {
  local_positions_.add(position);
}

void CalleePortFramesV2::set_local_positions(LocalPositionSet positions) {
  local_positions_ = std::move(positions);
}

void CalleePortFramesV2::add_inferred_features_and_local_position(
    const FeatureMayAlwaysSet& features,
    const Position* MT_NULLABLE position) {
  if (features.empty() && position == nullptr) {
    return;
  }

  map([&features](Frame& frame) {
    if (!features.empty()) {
      frame.add_inferred_features(features);
    }
  });

  if (position != nullptr) {
    add_local_position(position);
  }
}

CalleePortFramesV2 CalleePortFramesV2::propagate(
    const Method* callee,
    const AccessPath& callee_port,
    const Position* call_position,
    int maximum_source_sink_distance,
    Context& context,
    const std::vector<const DexType * MT_NULLABLE>& source_register_types,
    const std::vector<std::optional<std::string>>& source_constant_arguments)
    const {
  mt_assert(!is_result_or_receiver_sinks_);
  if (is_bottom()) {
    return CalleePortFramesV2::bottom();
  }

  CalleePortFramesV2 result;
  auto partitioned_by_kind = partition_map<const Kind*>(
      [](const Frame& frame) { return frame.kind(); });

  for (const auto& [kind, frames] : partitioned_by_kind) {
    if (callee_port_.root().is_anchor() && callee_port_.path().size() == 0) {
      // These are CRTEX leaf frames. CRTEX is identified by the "anchor" port,
      // leaf-ness is identified by the path() length. Once a CRTEX frame is
      // propagated, it's path is never empty.
      result.join_with(propagate_crtex_leaf_frames(
          callee,
          callee_port,
          call_position,
          maximum_source_sink_distance,
          context,
          source_register_types,
          frames));
    } else {
      std::vector<const Feature*> via_type_of_features_added;
      auto non_crtex_frame = propagate_frames(
          callee,
          callee_port,
          call_position,
          maximum_source_sink_distance,
          context,
          source_register_types,
          source_constant_arguments,
          frames,
          via_type_of_features_added);
      if (!non_crtex_frame.is_bottom()) {
        result.add(non_crtex_frame);
      }
    }
  }

  return result;
}

void CalleePortFramesV2::transform_kind_with_features(
    const std::function<std::vector<const Kind*>(const Kind*)>& transform_kind,
    const std::function<FeatureMayAlwaysSet(const Kind*)>& add_features) {
  // In practice, this method is never used for result/receiver sinks so we can
  // avoid considering the case where output paths should be updated if an
  // result/receiver sinks is transformed into a different kind.
  mt_assert(!is_result_or_receiver_sinks_);

  FramesByKind new_frames_by_kind;
  for (const auto& [old_kind, frames] : frames_.bindings()) {
    auto new_kinds = transform_kind(old_kind);
    if (new_kinds.empty()) {
      continue;
    } else if (new_kinds.size() == 1 && new_kinds.front() == old_kind) {
      new_frames_by_kind.set(old_kind, frames); // no transformation
    } else {
      for (const auto* new_kind : new_kinds) {
        // Even if new_kind == old_kind for some new_kind, perform map_frame_set
        // because a transformation occurred.
        Frames new_frames;
        auto features_to_add = add_features(new_kind);
        for (const auto& frame : frames) {
          auto new_frame = frame.with_kind(new_kind);
          new_frame.add_inferred_features(features_to_add);
          new_frames.add(new_frame);
        }
        new_frames_by_kind.update(new_kind, [&](const Frames& existing) {
          return existing.join(new_frames);
        });
      }
    }
  }

  frames_ = new_frames_by_kind;
}

void CalleePortFramesV2::filter_invalid_frames(
    const std::function<bool(const Method*, const AccessPath&, const Kind*)>&
        is_valid) {
  // This method is only used to post process inferred sources and sinks.
  // Since we don't use it for result/receiver sinks, we can avoid updating
  // `output_paths_` when frames are removed.
  mt_assert(!is_result_or_receiver_sinks_);
  FramesByKind new_frames;
  for (const auto& [kind, frames] : frames_.bindings()) {
    auto frames_copy = frames;
    frames_copy.filter([&](const Frame& frame) {
      return is_valid(frame.callee(), frame.callee_port(), frame.kind());
    });
    new_frames.set(kind, frames_copy);
  }

  frames_ = new_frames;
}

bool CalleePortFramesV2::contains_kind(const Kind* kind) const {
  for (const auto& [actual_kind, _] : frames_.bindings()) {
    if (actual_kind == kind) {
      return true;
    }
  }
  return false;
}

std::ostream& operator<<(std::ostream& out, const CalleePortFramesV2& frames) {
  if (frames.is_top()) {
    return out << "T";
  } else {
    out << "CalleePortFramesV2("
        << "callee_port=" << frames.callee_port()
        << ", is_result_or_receiver_sinks="
        << frames.is_result_or_receiver_sinks_;

    const auto& local_positions = frames.local_positions();
    if (!local_positions.is_bottom() && !local_positions.empty()) {
      out << ", local_positions=" << frames.local_positions();
    }

    if (!frames.output_paths_.is_bottom()) {
      out << ", output_paths=" << frames.output_paths_;
    }

    out << ", frames=[";
    for (const auto& [kind, frames] : frames.frames_.bindings()) {
      out << "FrameByKind(kind=" << show(kind) << ", frames=" << frames << "),";
    }
    return out << "])";
  }
}

void CalleePortFramesV2::add(const Frame& frame) {
  mt_assert(!frame.is_artificial_source());
  mt_assert(
      !frame.is_result_or_receiver_sink() ||
      frame.callee_port().path().empty());
  if (is_bottom()) {
    callee_port_ = frame.callee_port();
    is_result_or_receiver_sinks_ = frame.is_result_or_receiver_sink();
  } else {
    mt_assert(
        callee_port_ == frame.callee_port() &&
        is_result_or_receiver_sinks_ == frame.is_result_or_receiver_sink());
  }

  frames_.update(frame.kind(), [&](const Frames& old_frames) {
    auto new_frames = old_frames;
    new_frames.add(frame);
    return new_frames;
  });
}

Frame CalleePortFramesV2::propagate_frames(
    const Method* callee,
    const AccessPath& callee_port,
    const Position* call_position,
    int maximum_source_sink_distance,
    Context& context,
    const std::vector<const DexType * MT_NULLABLE>& source_register_types,
    const std::vector<std::optional<std::string>>& source_constant_arguments,
    std::vector<std::reference_wrapper<const Frame>> frames,
    std::vector<const Feature*>& via_type_of_features_added) const {
  if (frames.size() == 0) {
    return Frame::bottom();
  }

  const auto* kind = frames.begin()->get().kind();
  int distance = std::numeric_limits<int>::max();
  auto origins = MethodSet::bottom();
  auto field_origins = FieldSet::bottom();
  auto inferred_features = FeatureMayAlwaysSet::bottom();

  for (const Frame& frame : frames) {
    // Only frames sharing the same kind can be propagated this way.
    mt_assert(frame.kind() == kind);

    if (frame.distance() >= maximum_source_sink_distance) {
      continue;
    }

    distance = std::min(distance, frame.distance() + 1);
    origins.join_with(frame.origins());
    field_origins.join_with(frame.field_origins());

    // Note: This merges user features with existing inferred features.
    inferred_features.join_with(frame.features());

    materialize_via_type_of_ports(
        callee,
        context,
        frame,
        source_register_types,
        via_type_of_features_added,
        inferred_features);

    materialize_via_value_of_ports(
        callee, context, frame, source_constant_arguments, inferred_features);
  }

  if (distance == std::numeric_limits<int>::max()) {
    return Frame::bottom();
  }

  mt_assert(distance <= maximum_source_sink_distance);
  return Frame(
      kind,
      callee_port,
      callee,
      /* field_callee */ nullptr, // Since propagate is only called at method
                                  // callsites and not field accesses
      call_position,
      distance,
      std::move(origins),
      std::move(field_origins),
      std::move(inferred_features),
      /* locally_inferred_features */ FeatureMayAlwaysSet::bottom(),
      /* user_features */ FeatureSet::bottom(),
      /* via_type_of_ports */ {},
      /* via_value_of_ports */ {},
      /* canonical_names */ {});
}

CalleePortFramesV2 CalleePortFramesV2::propagate_crtex_leaf_frames(
    const Method* callee,
    const AccessPath& callee_port,
    const Position* call_position,
    int maximum_source_sink_distance,
    Context& context,
    const std::vector<const DexType * MT_NULLABLE>& source_register_types,
    std::vector<std::reference_wrapper<const Frame>> frames) const {
  if (frames.size() == 0) {
    return CalleePortFramesV2::bottom();
  }

  CalleePortFramesV2 result;
  const auto* kind = frames.begin()->get().kind();

  for (const Frame& frame : frames) {
    // Only frames sharing the same kind can be propagated this way.
    mt_assert(frame.kind() == kind);

    std::vector<const Feature*> via_type_of_features_added;
    auto propagated = propagate_frames(
        callee,
        callee_port,
        call_position,
        maximum_source_sink_distance,
        context,
        source_register_types,
        {}, // TODO: Support via-value-of for crtex frames
        {std::cref(frame)},
        via_type_of_features_added);

    if (propagated.is_bottom()) {
      continue;
    }

    auto canonical_names = frame.canonical_names();
    if (!canonical_names.is_value() || canonical_names.elements().empty()) {
      WARNING(
          2,
          "Encountered crtex frame without canonical names. Frame: `{}`",
          frame);
      continue;
    }

    CanonicalNameSetAbstractDomain instantiated_names;
    for (const auto& canonical_name : canonical_names.elements()) {
      auto instantiated_name = canonical_name.instantiate(
          propagated.callee(), via_type_of_features_added);
      if (!instantiated_name) {
        continue;
      }
      instantiated_names.add(*instantiated_name);
    }

    auto canonical_callee_port =
        propagated.callee_port().canonicalize_for_method(propagated.callee());

    // All fields should be propagated like other frames, except the crtex
    // fields. Ideally, origins should contain the canonical names as well,
    // but canonical names are strings and cannot be stored in MethodSet.
    // Frame is not propagated if none of the canonical names instantiated
    // successfully.
    if (instantiated_names.is_value() &&
        !instantiated_names.elements().empty()) {
      result.add(Frame(
          kind,
          canonical_callee_port,
          propagated.callee(),
          propagated.field_callee(),
          propagated.call_position(),
          /* distance (always leaves for crtex frames) */ 0,
          propagated.origins(),
          propagated.field_origins(),
          propagated.inferred_features(),
          propagated.locally_inferred_features(),
          propagated.user_features(),
          propagated.via_type_of_ports(),
          propagated.via_value_of_ports(),
          /* canonical_names */ instantiated_names));
    }
  }

  return result;
}

Json::Value CalleePortFramesV2::to_json(
    const Method* MT_NULLABLE callee,
    const Position* MT_NULLABLE position) const {
  auto taint = Json::Value(Json::objectValue);

  auto kinds = Json::Value(Json::arrayValue);
  for (const auto& [_, frames_by_kind] : frames_.bindings()) {
    // TODO(T91357916): `Frame` no longer needs to be stored in a
    // GroupHashedSet. The key to that hashed set is maintained outside of this
    // class by the `Taint` structure.
    mt_assert(frames_by_kind.size() == 1);
    for (const auto& frame : frames_by_kind) {
      kinds.append(frame.to_json(local_positions_));
    }
  }
  taint["kinds"] = kinds;

  if (callee != nullptr || position != nullptr ||
      !callee_port_.root().is_leaf()) {
    // In most cases, all 3 values (callee, position, port) are expected to be
    // present. Some edge cases are:
    //
    // - Standard leaf/terminal frames: The "call" key will be absent because
    //   there is no "next hop".
    // - CRTEX leaf/terminal frames: The callee port will be "producer/anchor".
    //   SAPP post-processing will transform it to something that other static
    //   analysis tools in the family can understand.
    // - Return sinks and parameter sources: There is no "callee", but the
    //   position points to the return instruction/parameter.
    auto call = Json::Value(Json::objectValue);

    if (callee != nullptr) {
      call["resolves_to"] = callee->to_json();
    }

    if (position != nullptr) {
      call["position"] = position->to_json();
    }

    if (!callee_port_.root().is_leaf()) {
      call["port"] = callee_port_.to_json();
    }

    taint["call"] = call;
  }

  return taint;
}

} // namespace marianatrench