// Copyright (c) by respective owners including Yahoo!, Microsoft, and
// individual contributors. All rights reserved. Released under a BSD (revised)
// license as described in the file LICENSE.

#include "reductions/marginal.h"

#include "config/options.h"
#include "correctedMath.h"
#include "example.h"
#include "global_data.h"
#include "interactions.h"
#include "io/logger.h"
#include "learner.h"
#include "loss_functions.h"
#include "setup_base.h"
#include "text_utils.h"

#include <map>
#include <unordered_map>

using namespace VW::config;
namespace
{
struct expert
{
  expert() = default;
  expert(float regret, float abs_regret, float weight) : regret(regret), abs_regret(abs_regret), weight(weight) {}

  float regret = 0.f;
  float abs_regret = 0.f;
  float weight = 1.f;
};

using marginal = std::pair<double, double>;
using expert_pair = std::pair<expert, expert>;

struct data
{
  data(float initial_numerator, float initial_denominator, float decay, bool update_before_learn,
      bool unweighted_marginals, bool compete, parameters* m_weights, VW::loss_function* m_loss_function,
      shared_data* m_shared_data, bool m_hash_inv, VW::io::logger logger)
      : initial_numerator(initial_numerator)
      , initial_denominator(initial_denominator)
      , decay(decay)
      , update_before_learn(update_before_learn)
      , unweighted_marginals(unweighted_marginals)
      , compete(compete)
      , m_weights(m_weights)
      , m_loss_function(m_loss_function)
      , m_shared_data(m_shared_data)
      , m_hash_inv(m_hash_inv)
      , m_logger(std::move(logger))
  {
    id_features.fill(false);
  }

  data(float initial_numerator, float initial_denominator, float decay, bool update_before_learn,
      bool unweighted_marginals, bool compete, VW::workspace& all)
      : data(initial_numerator, initial_denominator, decay, update_before_learn, unweighted_marginals, compete,
            &all.weights, all.loss.get(), all.sd, all.hash_inv, all.logger)
  {
  }

  float initial_numerator;
  float initial_denominator;
  float decay;
  bool update_before_learn;
  bool unweighted_marginals;

  std::array<bool, 256> id_features;
  std::array<features, 256> temp;  // temporary storage when reducing.
  std::map<uint64_t, marginal> marginals;

  // bookkeeping variables for experts
  bool compete;
  float feature_pred = 0.f;        // the prediction computed from using all the features
  float average_pred = 0.f;        // the prediction of the expert
  float net_weight = 0.f;          // normalizer for expert weights
  float net_feature_weight = 0.f;  // the net weight on the feature-based expert
  float alg_loss = 0.f;            // temporary storage for the loss of the current marginal-based predictor
  std::unordered_map<uint64_t, expert_pair>
      expert_state;  // pair of weights on marginal and feature based predictors, one per marginal feature

  parameters* m_weights;
  VW::loss_function* m_loss_function;
  shared_data* m_shared_data;
  bool m_hash_inv;
  std::unordered_map<uint64_t, std::string> inverse_hashes;
  VW::io::logger m_logger;
};

float get_adanormalhedge_weights(float r, float c)
{
  float r_pos = r > 0.f ? r : 0.f;
  if (c == 0.f || r_pos == 0.f) { return 0.f; }
  return 2.f * r_pos * correctedExp(r_pos * r_pos / (3.f * c)) / (3.f * c);
}

template <bool is_learn>
void make_marginal(data& sm, VW::example& ec)
{
  const uint64_t mask = sm.m_weights->mask();
  const float label = ec.l.simple.label;
  sm.alg_loss = 0.;
  sm.net_weight = 0.;
  sm.net_feature_weight = 0.;
  sm.average_pred = 0.;

  for (auto i = ec.begin(); i != ec.end(); ++i)
  {
    const VW::namespace_index n = i.index();
    if (sm.id_features[n])
    {
      std::swap(sm.temp[n], *i);
      features& f = *i;
      f.clear();
      size_t inv_hash_idx = 0;
      for (auto j = sm.temp[n].begin(); j != sm.temp[n].end(); ++j)
      {
        const float first_value = j.value();
        const uint64_t first_index = j.index() & mask;
        if (++j == sm.temp[n].end())
        {
          sm.m_logger.out_warn(
              "marginal feature namespace has {} features. Should be a multiple of 2", sm.temp[n].size());
          break;
        }
        const float second_value = j.value();
        const uint64_t second_index = j.index() & mask;
        if (first_value != 1.f || second_value != 1.f)
        {
          sm.m_logger.out_warn("Bad id features, must have value 1.");
          continue;
        }
        const uint64_t key = second_index + ec.ft_offset;
        if (sm.marginals.find(key) == sm.marginals.end())  // need to initialize things.
        {
          sm.marginals.insert(std::make_pair(key, std::make_pair(sm.initial_numerator, sm.initial_denominator)));
          if (sm.compete)
          {
            expert e = {0, 0, 1.};
            sm.expert_state.insert(std::make_pair(key, std::make_pair(e, e)));
          }
          if (sm.m_hash_inv)
          {
            std::ostringstream ss;
            std::vector<VW::audit_strings>& sn = sm.temp[n].space_names;
            ss << sn[inv_hash_idx].ns << "^" << sn[inv_hash_idx].name << "*" << sn[inv_hash_idx + 1].name;
            sm.inverse_hashes.insert(std::make_pair(key, ss.str()));
            inv_hash_idx += 2;
          }
        }
        const auto marginal_pred = static_cast<float>(sm.marginals[key].first / sm.marginals[key].second);
        f.push_back(marginal_pred, first_index);
        if (!sm.temp[n].space_names.empty()) { f.space_names.push_back(sm.temp[n].space_names[2 * (f.size() - 1)]); }

        if (sm.compete)  // compute the prediction from the marginals using the weights
        {
          float weight = sm.expert_state[key].first.weight;
          sm.average_pred += weight * marginal_pred;
          sm.net_weight += weight;
          sm.net_feature_weight += sm.expert_state[key].second.weight;
          if VW_STD17_CONSTEXPR (is_learn)
          { sm.alg_loss += weight * sm.m_loss_function->get_loss(sm.m_shared_data, marginal_pred, label); }
        }
      }
    }
  }
}

void undo_marginal(data& sm, VW::example& ec)
{
  for (auto i = ec.begin(); i != ec.end(); ++i)
  {
    const VW::namespace_index n = i.index();
    if (sm.id_features[n]) { std::swap(sm.temp[n], *i); }
  }
}

template <bool is_learn>
void compute_expert_loss(data& sm, VW::example& ec)
{
  // add in the feature-based expert and normalize,
  const float label = ec.l.simple.label;

  if (sm.net_weight + sm.net_feature_weight > 0.f) { sm.average_pred += sm.net_feature_weight * sm.feature_pred; }
  else
  {
    sm.net_feature_weight = 1.;
    sm.average_pred = sm.feature_pred;
  }
  const float inv_weight = 1.0f / (sm.net_weight + sm.net_feature_weight);
  sm.average_pred *= inv_weight;
  ec.pred.scalar = sm.average_pred;
  ec.partial_prediction = sm.average_pred;

  if VW_STD17_CONSTEXPR (is_learn)
  {
    sm.alg_loss += sm.net_feature_weight * sm.m_loss_function->get_loss(sm.m_shared_data, sm.feature_pred, label);
    sm.alg_loss *= inv_weight;
  }
}

void update_marginal(data& sm, VW::example& ec)
{
  const uint64_t mask = sm.m_weights->mask();
  const float label = ec.l.simple.label;
  float weight = ec.weight;
  if (sm.unweighted_marginals) { weight = 1.; }

  for (VW::example::iterator i = ec.begin(); i != ec.end(); ++i)
  {
    const VW::namespace_index n = i.index();
    if (sm.id_features[n])
    {
      for (auto j = sm.temp[n].begin(); j != sm.temp[n].end(); ++j)
      {
        if (++j == sm.temp[n].end()) { break; }

        const uint64_t second_index = j.index() & mask;
        uint64_t key = second_index + ec.ft_offset;
        marginal& m = sm.marginals[key];

        if (sm.compete)  // now update weights, before updating marginals
        {
          expert_pair& e = sm.expert_state[key];
          const float regret1 = sm.alg_loss -
              sm.m_loss_function->get_loss(sm.m_shared_data, static_cast<float>(m.first / m.second), label);
          const float regret2 = sm.alg_loss - sm.m_loss_function->get_loss(sm.m_shared_data, sm.feature_pred, label);

          e.first.regret += regret1 * weight;
          e.first.abs_regret += regret1 * regret1 * weight;  // fabs(regret1);
          e.first.weight = get_adanormalhedge_weights(e.first.regret, e.first.abs_regret);
          e.second.regret += regret2 * weight;
          e.second.abs_regret += regret2 * regret2 * weight;  // fabs(regret2);
          e.second.weight = get_adanormalhedge_weights(e.second.regret, e.second.abs_regret);
        }

        m.first = m.first * static_cast<double>(1.f - sm.decay) + static_cast<double>(ec.l.simple.label * weight);
        m.second = m.second * static_cast<double>(1.f - sm.decay) + static_cast<double>(weight);
      }
    }
  }
}

template <bool is_learn>
void predict_or_learn(data& sm, VW::LEARNER::single_learner& base, VW::example& ec)
{
  make_marginal<is_learn>(sm, ec);
  if VW_STD17_CONSTEXPR (is_learn)
  {
    if (sm.update_before_learn)
    {
      base.predict(ec);
      const float pred = ec.pred.scalar;
      if (sm.compete)
      {
        sm.feature_pred = pred;
        compute_expert_loss<is_learn>(sm, ec);
      }
      undo_marginal(sm, ec);
      update_marginal(sm, ec);  // update features before learning.
      make_marginal<is_learn>(sm, ec);
      base.learn(ec);
      ec.pred.scalar = pred;
    }
    else
    {
      base.learn(ec);
      if (sm.compete)
      {
        sm.feature_pred = ec.pred.scalar;
        compute_expert_loss<is_learn>(sm, ec);
      }
      update_marginal(sm, ec);
    }
  }
  else
  {
    base.predict(ec);
    const float pred = ec.pred.scalar;
    if (sm.compete)
    {
      sm.feature_pred = pred;
      compute_expert_loss<is_learn>(sm, ec);
    }
  }

  // undo marginalization
  undo_marginal(sm, ec);
}

void save_load(data& sm, io_buf& io, bool read, bool text)
{
  const uint64_t stride_shift = sm.m_weights->stride_shift();

  if (io.num_files() == 0) { return; }
  std::stringstream msg;
  uint64_t total_size;
  if (!read)
  {
    total_size = static_cast<uint64_t>(sm.marginals.size());
    msg << "marginals size = " << total_size << "\n";
  }
  bin_text_read_write_fixed_validated(io, reinterpret_cast<char*>(&total_size), sizeof(total_size), read, msg, text);

  auto iter = sm.marginals.begin();
  for (size_t i = 0; i < total_size; ++i)
  {
    uint64_t index;
    if (!read)
    {
      index = iter->first >> stride_shift;
      if (sm.m_hash_inv) { msg << sm.inverse_hashes[iter->first]; }
      else
      {
        msg << index;
      }
      msg << ":";
    }
    bin_text_read_write_fixed(io, reinterpret_cast<char*>(&index), sizeof(index), read, msg, text);
    double numerator;
    if (!read)
    {
      numerator = iter->second.first;
      msg << numerator << ":";
    }
    bin_text_read_write_fixed(io, reinterpret_cast<char*>(&numerator), sizeof(numerator), read, msg, text);
    double denominator;
    if (!read)
    {
      denominator = iter->second.second;
      msg << denominator << "\n";
    }
    bin_text_read_write_fixed(io, reinterpret_cast<char*>(&denominator), sizeof(denominator), read, msg, text);
    if (read) { sm.marginals.insert(std::make_pair(index << stride_shift, std::make_pair(numerator, denominator))); }
    else
    {
      ++iter;
    }
  }

  if (sm.compete)
  {
    if (!read)
    {
      total_size = static_cast<uint64_t>(sm.expert_state.size());
      msg << "expert_state size = " << total_size << "\n";
    }
    bin_text_read_write_fixed_validated(io, reinterpret_cast<char*>(&total_size), sizeof(total_size), read, msg, text);

    auto exp_iter = sm.expert_state.begin();
    for (size_t i = 0; i < total_size; ++i)
    {
      uint64_t index;
      if (!read)
      {
        index = exp_iter->first >> stride_shift;
        msg << index << ":";
      }
      bin_text_read_write_fixed(io, reinterpret_cast<char*>(&index), sizeof(index), read, msg, text);
      float r1 = 0;
      float c1 = 0;
      float w1 = 0;
      float r2 = 0;
      float c2 = 0;
      float w2 = 0;
      if (!read)
      {
        r1 = exp_iter->second.first.regret;
        c1 = exp_iter->second.first.abs_regret;
        w1 = exp_iter->second.first.weight;
        r2 = exp_iter->second.second.regret;
        c2 = exp_iter->second.second.abs_regret;
        w2 = exp_iter->second.second.weight;
        msg << r1 << ":";
      }
      bin_text_read_write_fixed(io, reinterpret_cast<char*>(&r1), sizeof(r1), read, msg, text);
      if (!read) { msg << c1 << ":"; }
      bin_text_read_write_fixed(io, reinterpret_cast<char*>(&c1), sizeof(c1), read, msg, text);
      if (!read) { msg << w1 << ":"; }
      bin_text_read_write_fixed(io, reinterpret_cast<char*>(&w1), sizeof(w1), read, msg, text);
      if (!read) { msg << r2 << ":"; }
      bin_text_read_write_fixed(io, reinterpret_cast<char*>(&r2), sizeof(r2), read, msg, text);
      if (!read) { msg << c2 << ":"; }
      bin_text_read_write_fixed(io, reinterpret_cast<char*>(&c2), sizeof(c2), read, msg, text);
      if (!read) { msg << w2 << ":"; }
      bin_text_read_write_fixed(io, reinterpret_cast<char*>(&w2), sizeof(w2), read, msg, text);

      if (read)
      {
        expert e1 = {r1, c1, w1};
        expert e2 = {r2, c2, w2};
        sm.expert_state.insert(std::make_pair(index << stride_shift, std::make_pair(e1, e2)));
      }
      else
      {
        ++exp_iter;
      }
    }
  }
}
}  // namespace

VW::LEARNER::base_learner* VW::reductions::marginal_setup(VW::setup_base_i& stack_builder)
{
  options_i& options = *stack_builder.get_options();
  VW::workspace* all = stack_builder.get_all_pointer();

  std::string marginal;
  float initial_denominator;
  float initial_numerator;
  bool compete = false;
  bool update_before_learn = false;
  bool unweighted_marginals = false;
  float decay;
  option_group_definition marginal_options("[Reduction] Marginal");
  marginal_options
      .add(make_option("marginal", marginal).keep().necessary().help("Substitute marginal label estimates for ids"))
      .add(make_option("initial_denominator", initial_denominator).default_value(1.f).help("Initial denominator"))
      .add(make_option("initial_numerator", initial_numerator).default_value(0.5f).help("Initial numerator"))
      .add(make_option("compete", compete).help("Enable competition with marginal features"))
      .add(make_option("update_before_learn", update_before_learn).help("Update marginal values before learning"))
      .add(make_option("unweighted_marginals", unweighted_marginals)
               .help("Ignore importance weights when computing marginals"))
      .add(make_option("decay", decay).default_value(0.f).help("Decay multiplier per event (1e-3 for example)"));

  if (!options.add_parse_and_check_necessary(marginal_options)) { return nullptr; }

  auto d = VW::make_unique<::data>(
      initial_numerator, initial_denominator, decay, update_before_learn, unweighted_marginals, compete, *all);

  marginal = VW::decode_inline_hex(marginal, all->logger);
  if (marginal.find(':') != std::string::npos) { THROW("Cannot use wildcard with marginal.") }
  for (const auto ns : marginal) { d->id_features[static_cast<unsigned char>(ns)] = true; }

  auto* l = VW::LEARNER::make_reduction_learner(std::move(d), as_singleline(stack_builder.setup_base_learner()),
      predict_or_learn<true>, predict_or_learn<false>, stack_builder.get_setupfn_name(marginal_setup))
                .set_input_label_type(VW::label_type_t::simple)
                .set_output_prediction_type(VW::prediction_type_t::scalar)
                .set_learn_returns_prediction(true)
                .set_save_load(save_load)
                .build();

  return make_base(*l);
}
