#include "./scitree_nif_helper.hpp"
#include "./scitree_dataset.hpp"
#include "./scitree_learner.hpp"

#include "absl/flags/flag.h"
#include "yggdrasil_decision_forests/utils/filesystem.h"
#include "yggdrasil_decision_forests/dataset/data_spec.pb.h"
#include "yggdrasil_decision_forests/utils/logging.h"
#include "yggdrasil_decision_forests/dataset/data_spec.h"
#include "yggdrasil_decision_forests/dataset/data_spec_inference.h"
#include "yggdrasil_decision_forests/dataset/vertical_dataset_io.h"
#include "yggdrasil_decision_forests/metric/metric.h"
#include "yggdrasil_decision_forests/metric/report.h"
#include "yggdrasil_decision_forests/model/model_library.h"

#include <map>
#include <vector>
#include <cstring>
#include <erl_nif.h>

ErlNifResourceType *RES_TYPE;

namespace ygg = yggdrasil_decision_forests;

static int open_resource(ErlNifEnv *env) {
  const char *mod = "resources";
  const char *name = "yggdrasil";
  int flags = ERL_NIF_RT_CREATE | ERL_NIF_RT_TAKEOVER;

  RES_TYPE = enif_open_resource_type(env, mod, name, NULL, (ErlNifResourceFlags)flags, NULL);
  if (RES_TYPE == NULL)
    return -1;
  return 0;
}

static int load(ErlNifEnv *env, void **priv, ERL_NIF_TERM load_info) {
  absl::SetFlag(&FLAGS_alsologtostderr, false);
  if (open_resource(env) == -1)
    return -1;

  return 0;
}

static ERL_NIF_TERM train(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  scitree::nif::SCITREE_CONFIG config = scitree::nif::make_scitree_config(env, argv[0]);

  if (config.error.status) {
    return scitree::nif::error(env, config.error.reason.c_str());
  }

  int size_dataset;
  ERL_NIF_TERM* tuple_dataset;

  enif_get_tuple(env, argv[1], &size_dataset, &tuple_dataset);

  if (size_dataset <= 0) {
    return scitree::nif::error(env, "Empty or invalid dataset.");
  }

  // Training configuration
  ygg::model::proto::TrainingConfig train_config;
  train_config.set_learner(config.learner);
  train_config.set_task(config.task);
  train_config.set_label(config.label);
  
  // Create types dataspec
  ygg::dataset::proto::DataSpecification spec;
  ygg::dataset::VerticalDataset dataset;

  auto error_spec = scitree::dataset::load_data_spec(&spec, env, tuple_dataset, size_dataset);
  if (error_spec.status) {
    return scitree::nif::error(env, error_spec.reason.c_str());
  }

  auto error_dataset = scitree::dataset::load_dataset(&dataset, &spec, env, tuple_dataset, size_dataset);
  if (error_dataset.status) {
    return scitree::nif::error(env, error_dataset.reason.c_str());
  }

  LOG(INFO) << "Training dataset:\n"
            << ygg::dataset::PrintHumanReadable(dataset.data_spec(), false);

  // Config leaener
  std::unique_ptr<ygg::model::AbstractLearner> learner;
  GetLearner(train_config, &learner);

  if (config.log_directory.length() > 0)
    learner->set_log_directory(config.log_directory);

  // Define options
  learner->SetHyperParameters(scitree::learner::get_hyper_params(config.options));

  auto model = learner->TrainWithStatus(dataset).value();
  ygg::model::AbstractModel **p_model;
  p_model = (ygg::model::AbstractModel **)enif_alloc_resource(RES_TYPE, sizeof(ygg::model::AbstractModel *));

  *p_model = model.release();

  if (*p_model == NULL)
    return scitree::nif::error(env, "Unable to open resource.");

  ERL_NIF_TERM resource = enif_make_resource(env, p_model);

  return enif_make_tuple2(env, scitree::nif::ok(env), resource);
}

static ERL_NIF_TERM predict(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  ygg::model::AbstractModel **p_model;

  if (!enif_get_resource(env, argv[0], RES_TYPE, (void **)&p_model)) {
    return scitree::nif::error(env, "Unable to load resource.");
  }

  int size_dataset;
  ERL_NIF_TERM* tuple_dataset;

  enif_get_tuple(env, argv[1], &size_dataset, &tuple_dataset);
  
  if (size_dataset <= 0) {
    return scitree::nif::error(env, "Empty or invalid dataset.");
  }

  // Create types dataspec
  ygg::dataset::VerticalDataset dataset_predit;
  ygg::dataset::proto::DataSpecification spec = (**p_model).data_spec();

  // Load dataset
  auto error_dataset = scitree::dataset::load_dataset(&dataset_predit, &spec, env, tuple_dataset, size_dataset);
  if (error_dataset.status) {
    return scitree::nif::error(env, error_dataset.reason.c_str());
  }

  // Will compile the model into the most efficient engine
  // on the current hardware.
  const std::unique_ptr<ygg::serving::FastEngine> serving_engine =
      (**p_model).BuildFastEngine().value();
  const auto &features = serving_engine->features();

  int count = dataset_predit.nrow();
  std::unique_ptr<ygg::serving::AbstractExampleSet> examples =
      serving_engine->AllocateExamples(count);

  ygg::serving::CopyVerticalDatasetToAbstractExampleSet(
      dataset_predit, 0, count -1, features, examples.get());
  
  std::vector<float> batch_of_predictions;
  serving_engine->Predict(*examples, count, &batch_of_predictions);

  ERL_NIF_TERM predictions[batch_of_predictions.size()];
  int i = 0;
  for (float const &predict : batch_of_predictions) {
    predictions[i++] = enif_make_double(env, predict);
  }

  ERL_NIF_TERM list = enif_make_list_from_array(env, predictions, batch_of_predictions.size());

  return enif_make_tuple2(env, scitree::nif::ok(env), list);
}

static ERL_NIF_TERM save(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  ygg::model::AbstractModel **p_model;

  std::string path;

  if (!scitree::nif::get(env, argv[1], path)) {
    return scitree::nif::error(env, "Unable to get path.");
  }

  if (!enif_get_resource(env, argv[0], RES_TYPE, (void **)&p_model)) {
    return scitree::nif::error(env, "Unable to load resource.");
  }

  SaveModel(path, *p_model);

  return scitree::nif::ok(env);
}

static ErlNifFunc nif_funcs[] = {
    {"train", 2, train},
    {"predict", 2, predict},
    {"save", 2, save}};

ERL_NIF_INIT(Elixir.Scitree.Native, nif_funcs, &load, NULL, NULL, NULL)
