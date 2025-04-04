/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <Show.h>
#include <TypeUtil.h>

#include <json/value.h>

#include <mariana-trench/Assert.h>
#include <mariana-trench/ClassIntervals.h>
#include <mariana-trench/JsonReaderWriter.h>
#include <mariana-trench/JsonValidation.h>
#include <mariana-trench/Log.h>

namespace marianatrench {

namespace {

// Sets the class interval for `current_node` while performing a DFS on it.
void dfs_on_hierarchy(
    const ClassHierarchy& class_hierarchy,
    const DexType* current_node,
    std::int32_t& dfs_order,
    std::unordered_map<const DexType*, ClassIntervals::Interval>& result) {
  auto lower_bound = dfs_order;

  auto children = class_hierarchy.find(current_node);
  mt_assert(children != class_hierarchy.end());

  for (const auto* child : children->second) {
    mt_assert(
        dfs_order <
        std::numeric_limits<int32_t>::max()); // Ensure no overflows.
    ++dfs_order;
    dfs_on_hierarchy(class_hierarchy, child, dfs_order, result);
  }

  mt_assert(
      dfs_order < std::numeric_limits<int32_t>::max()); // Ensure no overflows.
  ++dfs_order;

  // Each node should only be visited once since multiple inheritance is not
  // supported by Java/Kotlin.
  mt_assert(result.find(current_node) == result.end());
  auto interval = ClassIntervals::Interval::finite(lower_bound, dfs_order);
  result.emplace(current_node, interval);
}

ClassIntervals::ClassIntervalsMap read_class_intervals(
    const std::filesystem::path& class_intervals_file) {
  if (!std::filesystem::exists(class_intervals_file)) {
    throw std::runtime_error("Class intervals file must exist.");
  }

  LOG(1, "Reading class intervals from `{}`", class_intervals_file.native());

  auto class_intervals_json = JsonReader::parse_json_file(class_intervals_file);
  return ClassIntervals::from_json(class_intervals_json);
}

} // namespace

ClassIntervals::ClassIntervals(
    const Options& options,
    AnalysisMode analysis_mode,
    const DexStoresVector& stores)
    : top_(Interval::top()) {
  switch (analysis_mode) {
    case AnalysisMode::Normal:
      init_from_stores(stores);
      break;
    case AnalysisMode::CachedModels:
      init_from_stores(stores);
      // Cached intervals are ignored. Consider re-computing them. Requires
      // handling cases where an internal class extends an external class that
      // already has an interval in the cached output. Intervals in the cached
      // models need to be re-mapped too.
      break;
    case AnalysisMode::Replay: {
      // Do not recompute intervals in replay mode.
      auto class_intervals_input_path = options.class_intervals_input_path();
      mt_assert(class_intervals_input_path.has_value());
      class_intervals_ = read_class_intervals(*class_intervals_input_path);
      break;
    }
    default:
      mt_unreachable();
  }

  if (options.dump_class_intervals()) {
    auto class_intervals_path = options.class_intervals_output_path();
    LOG(1, "Writing class intervals to `{}`", class_intervals_path.native());
    JsonWriter::write_json_file(class_intervals_path, to_json());

    // Dumping class intervals is test-only, perform additional, otherwise
    // unnecessary/expensive validation here.
    for (const auto& scope : DexStoreClassesIterator(stores)) {
      for (const auto& klass : scope) {
        if (!is_interface(klass) &&
            class_intervals_.find(klass->get_type()) ==
                class_intervals_.end()) {
          // Might happen if not everything was rooted in Object.
          WARNING(1, "Did not compute interval for `{}`.", show(klass));
        }
      }
    }
  }
}

const ClassIntervals::Interval& ClassIntervals::get_interval(
    const DexType* type) const {
  auto interval = class_intervals_.find(type);
  if (interval == class_intervals_.end()) {
    // Type not found. Use top to represent the broadest possible type.
    return top_;
  }

  return interval->second;
}

Json::Value ClassIntervals::interval_to_json(const Interval& interval) {
  auto interval_json = Json::Value(Json::arrayValue);
  if (interval.is_bottom()) {
    // Empty array for bottom interval.
    return interval_json;
  }

  // When this is read back in `interval_from_json()`, it should have Json::Int
  // type. Note that comparing a Json::UInt type against a Json::Int type will
  // fail equality checks even for the same integer value.
  interval_json.append(Json::Value(interval.lower_bound()));
  interval_json.append(Json::Value(interval.upper_bound()));

  return interval_json;
}

ClassIntervals::Interval ClassIntervals::interval_from_json(
    const Json::Value& value) {
  auto bounds = JsonValidation::null_or_array(value);
  if (bounds.empty()) {
    return Interval::bottom();
  }

  if (bounds.size() != 2) {
    throw JsonValidationError(
        value, /* field */ std::nullopt, "array of size 2 for class interval");
  }

  auto lower_bound = JsonValidation::integer(bounds[0]);
  auto upper_bound = JsonValidation::integer(bounds[1]);

  if (lower_bound == Interval::MIN && upper_bound == Interval::MAX) {
    return Interval::top();
  } else if (lower_bound == Interval::MIN) {
    return Interval::bounded_above(upper_bound);
  } else if (upper_bound == Interval::MAX) {
    return Interval::bounded_below(lower_bound);
  }

  return Interval::finite(lower_bound, upper_bound);
}

Json::Value ClassIntervals::to_json() const {
  auto output = Json::Value(Json::objectValue);
  for (auto [klass, interval] : class_intervals_) {
    output[show(klass)] = interval_to_json(interval);
  }
  return output;
}

ClassIntervals::ClassIntervalsMap ClassIntervals::from_json(
    const Json::Value& value) {
  std::unordered_map<const DexType*, Interval> class_intervals;
  for (const auto& klass : value.getMemberNames()) {
    auto interval = interval_from_json(value[klass]);
    class_intervals.emplace(DexType::get_type(klass), interval);
  }
  return class_intervals;
}

void ClassIntervals::init_from_stores(const DexStoresVector& stores) {
  // Theoretically, all classes are rooted in java.lang.Object. In practice,
  // internal classes inheriting from external classes will not be reachable
  // from Object. Treat the class hierarchy as a forest of trees, with roots
  // being either direct children of Object or internal classes deriving
  // directly from external classes.
  ClassHierarchy class_hierarchy;
  const auto* object_root = type::java_lang_Object();

  // Use a deterministic ordering for the roots so that integration tests always
  // return the same interval for the same class.
  std::set<const DexType*, dextypes_comparator> roots;

  for (const auto& scope : DexStoreClassesIterator(stores)) {
    auto store_hierarchy = build_type_hierarchy(scope);
    for (const auto& [parent, children] : UnorderedIterable(store_hierarchy)) {
      if (parent == object_root) {
        roots.insert(children.begin(), children.end());
      } else {
        const auto* parent_class = type_class(parent);
        if (parent_class == nullptr) {
          // DexType exists but not DexClass, this is an external class.
          continue;
        }

        class_hierarchy[parent].insert(children.begin(), children.end());

        const auto* super_class = type_class(parent_class->get_super_class());
        if (super_class == nullptr) {
          // DexClass does not exist for for the super class. This is a root
          // (derives directly from an external class).
          roots.insert(parent_class->get_type());
        }
      }
    }
  }

  std::int32_t dfs_order = MIN_INTERVAL;
  for (const auto* root : roots) {
    dfs_on_hierarchy(class_hierarchy, root, dfs_order, class_intervals_);
    mt_assert(
        dfs_order <
        std::numeric_limits<int32_t>::max()); // Ensure no overflows.
    ++dfs_order;
  }
}

} // namespace marianatrench
