/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/filesystem/path.hpp>
#include <json/json.h>

#include <ConcurrentContainers.h>
#include <DexClass.h>
#include <DexStore.h>

#include <mariana-trench/Context.h>
#include <mariana-trench/FieldModel.h>
#include <mariana-trench/IncludeMacros.h>
#include <mariana-trench/LiteralModel.h>
#include <mariana-trench/Model.h>

namespace marianatrench {

class MethodMappings;

class Registry final {
 public:
  /* Create a registry with default models for all methods. */
  explicit Registry(Context& context);

  /* Create a registry with the given models for methods and fields. */
  explicit Registry(
      Context& context,
      const std::vector<Model>& models,
      const std::vector<FieldModel>& field_models,
      const std::vector<LiteralModel>& literal_models);

  /* Create a registry with the given json models. */
  explicit Registry(
      Context& context,
      const Json::Value& models_value,
      const Json::Value& field_models_value,
      const Json::Value& literal_models_value);

  /* Create a registry with the given models. */
  explicit Registry(
      Context& context,
      ConcurrentMap<const Method*, Model>&& models,
      ConcurrentMap<const Field*, FieldModel>&& field_models,
      ConcurrentMap<std::string, LiteralModel>&& literal_models);

  MOVE_CONSTRUCTOR_ONLY(Registry);

  /**
   * Load the global registry
   *
   * This joins all generated models and json models.
   * Afterwards, it creates a default model for methods that don't have one.
   */
  static Registry load(
      Context& context,
      const Options& options,
      AnalysisMode analysis_mode,
      MethodMappings method_mappings);

  void add_default_models();

  /* These are thread-safe. */
  Model get(const Method* method) const;
  FieldModel get(const Field* field) const;

  /* This is thread-safe. */
  bool has_model(const Method* method) const;

  /* This is thread-safe. */
  void set(const Model& model);

  /*
   * Checks the given literal against each configured literal model and returns
   * the combined model of all matched models.
   *
   * @param literal String literal for which to provide a model.
   * @return Combined literal model of all matched models.
   */
  LiteralModel match_literal(std::string_view literal) const;

  std::size_t models_size() const;
  std::size_t field_models_size() const;
  std::size_t literal_models_size() const;
  std::size_t issues_size() const;

  void join_with(const Model& model);
  void join_with(const FieldModel& field_model);
  void join_with(const LiteralModel& literal_model);
  void join_with(const Registry& other);

  void dump_metadata(const std::filesystem::path& path) const;

  void to_sharded_models_json(
      const std::filesystem::path& path,
      const std::size_t shard_limit =
          JsonValidation::k_default_shard_limit) const;

  const ConcurrentMap<const Method*, Model>& models() const {
    return models_;
  }

  const ConcurrentMap<const Field*, FieldModel>& field_models() const {
    return field_models_;
  }

  const ConcurrentMap<std::string, LiteralModel>& literal_models() const {
    return literal_models_;
  }

  void verify_expected_output(
      const std::filesystem::path& test_output_path) const;

  std::string dump_models() const;
  Json::Value models_to_json() const;

 private:
  Context& context_;

  ConcurrentMap<const Method*, Model> models_;
  ConcurrentMap<const Field*, FieldModel> field_models_;
  ConcurrentMap<std::string, LiteralModel> literal_models_;
};

} // namespace marianatrench
