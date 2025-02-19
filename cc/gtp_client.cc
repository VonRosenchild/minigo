// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cc/gtp_client.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>

#include "absl/strings/numbers.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "cc/constants.h"
#include "cc/file/utils.h"
#include "cc/logging.h"
#include "cc/sgf.h"

namespace minigo {

GtpClient::GtpClient(std::unique_ptr<ModelFactory> model_factory,
                     std::shared_ptr<InferenceCache> inference_cache,
                     const std::string& model_descriptor,
                     const Game::Options& game_options,
                     const MctsPlayer::Options& player_options,
                     const GtpClient::Options& client_options)
    : model_factory_(std::move(model_factory)),
      inference_cache_(inference_cache),
      options_(client_options) {
  auto model = model_factory_->NewModel(model_descriptor);
  game_ = absl::make_unique<Game>(model->name(), model->name(), game_options);

  // Create the main player. Its model doesn't run through the batcher used for
  // background inferences.
  player_ = absl::make_unique<MctsPlayer>(std::move(model), inference_cache,
                                          game_.get(), player_options);

  if (options_.ponder_limit > 0) {
    ponder_type_ = PonderType::kReadLimited;
  }
  RegisterCmd("benchmark", &GtpClient::HandleBenchmark);
  RegisterCmd("boardsize", &GtpClient::HandleBoardsize);
  RegisterCmd("clear_board", &GtpClient::HandleClearBoard);
  RegisterCmd("final_score", &GtpClient::HandleFinalScore);
  RegisterCmd("genmove", &GtpClient::HandleGenmove);
  RegisterCmd("known_command", &GtpClient::HandleKnownCommand);
  RegisterCmd("komi", &GtpClient::HandleKomi);
  RegisterCmd("list_commands", &GtpClient::HandleListCommands);
  RegisterCmd("loadsgf", &GtpClient::HandleLoadsgf);
  RegisterCmd("name", &GtpClient::HandleName);
  RegisterCmd("play", &GtpClient::HandlePlay);
  RegisterCmd("ponder", &GtpClient::HandlePonder);
  RegisterCmd("readouts", &GtpClient::HandleReadouts);
  RegisterCmd("showboard", &GtpClient::HandleShowboard);
  RegisterCmd("undo", &GtpClient::HandleUndo);
}

GtpClient::~GtpClient() = default;

void GtpClient::Run() {
  // Perform a warm-up inference: ML frameworks like TensorFlow often perform
  // lazy initialization, causing the first inference to take substantially
  // longer than subsequent ones, which can interfere with time keeping.
  MG_LOG(INFO) << "Warming up...";
  Position position(Color::kBlack);
  ModelOutput output;
  ModelInput input;
  input.sym = symmetry::kIdentity;
  input.position_history.push_back(&position);
  std::vector<const ModelInput*> inputs = {&input};
  std::vector<ModelOutput*> outputs = {&output};
  player_->model()->RunMany(inputs, &outputs, nullptr);
  MG_LOG(INFO) << "GTP engine ready";

  // Start a background thread that pushes lines read from stdin into the
  // thread safe stdin_queue_. This allows us to ponder when there's nothing
  // to read from stdin.
  std::atomic<bool> running(true);
  std::thread stdin_thread([this, &running]() {
    std::string line;
    while (std::cin) {
      std::getline(std::cin, line);
      stdin_queue_.Push(line);
    }
    running = false;
  });

  // Don't wait for the stdin reading thread to exit because there's no way to
  // abort the blocking call std::getline read (apart from the user hitting
  // ctrl-C). The OS will clean the thread up when the process exits.
  stdin_thread.detach();

  NewGame();

  while (running) {
    std::string line;

    // If there's a command waiting on stdin, process it.
    if (stdin_queue_.TryPop(&line)) {
      auto response = HandleCmd(line);
      std::cout << response << std::flush;
      if (response.done) {
        break;
      }
      continue;
    }

    // Otherwise, ponder if enabled.
    if (!MaybePonder()) {
      // If pondering isn't enabled, try and pop a command from stdin with a
      // short timeout. The timeout gives us a chance to break out of the loop
      // when stdin is closed with ctrl-C.
      if (stdin_queue_.PopWithTimeout(&line, absl::Seconds(1))) {
        auto response = HandleCmd(line);
        std::cout << response << std::flush;
        if (response.done) {
          break;
        }
      }
    }
  }
  running = false;
}

void GtpClient::NewGame() {
  player_->NewGame();
  MaybeStartPondering();
}

void GtpClient::MaybeStartPondering() {
  if (ponder_type_ != PonderType::kOff) {
    ponder_limit_reached_ = false;
    ponder_read_count_ = 0;
    if (ponder_type_ == PonderType::kTimeLimited) {
      ponder_time_limit_ = absl::Now() + ponder_duration_;
    }
  }
}

bool GtpClient::MaybePonder() {
  if (player_->root()->game_over() || ponder_type_ == PonderType::kOff ||
      ponder_limit_reached_) {
    return false;
  }

  // Check if we're finished pondering.
  if ((ponder_type_ == PonderType::kReadLimited &&
       ponder_read_count_ >= options_.ponder_limit) ||
      (ponder_type_ == PonderType::kTimeLimited &&
       absl::Now() >= ponder_time_limit_)) {
    if (!ponder_limit_reached_) {
      MG_LOG(INFO) << "mg-ponder: done";
      ponder_limit_reached_ = true;
    }
    return false;
  }

  Ponder();

  return true;
}

void GtpClient::Ponder() {
  // Remember the number of reads at the root.
  int n = player_->root()->N();

  player_->TreeSearch(player_->options().virtual_losses,
                      std::numeric_limits<int>::max());

  // Increment the ponder count by difference new and old reads.
  ponder_read_count_ += player_->root()->N() - n;
}

GtpClient::Response GtpClient::ReplaySgf(
    const std::vector<std::unique_ptr<sgf::Node>>& trees) {
  if (!trees.empty()) {
    // the SGF parser takes care of transforming an sgf into moves that the
    // engine is able to understand, so all we do here is just play them in.
    for (const auto& move : trees[0]->ExtractMainLine()) {
      if (!player_->PlayMove(move.c)) {
        MG_LOG(ERROR) << "Couldn't play move " << move.c;
        return Response::Error("Cannot load file");
      }
    }
  }
  return Response::Ok();
}

GtpClient::Response GtpClient::HandleCmd(const std::string& line) {
  std::vector<absl::string_view> args =
      absl::StrSplit(line, absl::ByAnyChar(" \t\r\n"), absl::SkipWhitespace());
  if (args.empty()) {
    return Response::Ok();
  }

  // Split the GTP into possible ID, command and arguments.
  int cmd_id;
  bool has_cmd_id = absl::SimpleAtoi(args[0], &cmd_id);
  if (has_cmd_id) {
    args.erase(args.begin());
  }
  auto cmd = std::string(args[0]);
  args.erase(args.begin());

  // Process the command.
  Response response;
  if (cmd == "quit") {
    response = Response::Done();
  } else {
    response = DispatchCmd(cmd, args);
  }

  // Set the command ID on the response if we have one.
  if (has_cmd_id) {
    response.set_cmd_id(cmd_id);
  }
  return response;
}

GtpClient::Response GtpClient::CheckArgsExact(size_t expected_num_args,
                                              CmdArgs args) {
  if (args.size() != expected_num_args) {
    return Response::Error("expected ", expected_num_args, " args, got ",
                           args.size(), " args: ", absl::StrJoin(args, " "));
  }
  return Response::Ok();
}

GtpClient::Response GtpClient::CheckArgsRange(size_t expected_min_args,
                                              size_t expected_max_args,
                                              CmdArgs args) {
  if (args.size() < expected_min_args || args.size() > expected_max_args) {
    return Response::Error("expected between ", expected_min_args, " and ",
                           expected_max_args, " args, got ", args.size(),
                           " args: ", absl::StrJoin(args, " "));
  }
  return Response::Ok();
}

GtpClient::Response GtpClient::DispatchCmd(const std::string& cmd,
                                           CmdArgs args) {
  auto it = cmd_handlers_.find(cmd);
  if (it == cmd_handlers_.end()) {
    return Response::Error("unknown command");
  }
  return it->second(args);
}

GtpClient::Response GtpClient::HandleBenchmark(CmdArgs args) {
  // benchmark [readouts] [virtual_losses]
  // Note: By default use current time_control (readouts or time).
  auto response = CheckArgsRange(0, 2, args);
  if (!response.ok) {
    return response;
  }

  auto saved_options = player_->options();
  auto temp_options = saved_options;

  if (args.size() > 0) {
    temp_options.seconds_per_move = 0;
    if (!absl::SimpleAtoi(args[0], &temp_options.num_readouts)) {
      return Response::Error("bad num_readouts");
    }
  }

  if (args.size() == 2) {
    if (!absl::SimpleAtoi(args[1], &temp_options.virtual_losses)) {
      return Response::Error("bad virtual_losses");
    }
  }

  player_->SetOptions(temp_options);
  player_->SuggestMove(temp_options.num_readouts);
  player_->SetOptions(saved_options);

  return Response::Ok();
}

GtpClient::Response GtpClient::HandleBoardsize(CmdArgs args) {
  auto response = CheckArgsExact(1, args);
  if (!response.ok) {
    return response;
  }

  int x;
  if (!absl::SimpleAtoi(args[0], &x) || x != kN) {
    return Response::Error("unacceptable size");
  }

  return Response::Ok();
}

GtpClient::Response GtpClient::HandleClearBoard(CmdArgs args) {
  auto response = CheckArgsExact(0, args);
  if (!response.ok) {
    return response;
  }
  NewGame();
  return Response::Ok();
}

GtpClient::Response GtpClient::HandleFinalScore(CmdArgs args) {
  auto response = CheckArgsExact(0, args);
  if (!response.ok) {
    return response;
  }
  if (!game_->game_over()) {
    // Game isn't over yet, calculate the current score using Tromp-Taylor
    // scoring.
    return Response::Ok(Game::FormatScore(
        player_->root()->position.CalculateScore(game_->options().komi)));
  } else {
    // Game is over, we have the result available.
    return Response::Ok(game_->result_string());
  }
}

GtpClient::Response GtpClient::HandleGenmove(CmdArgs args) {
  auto response = CheckArgsRange(0, 1, args);
  if (!response.ok) {
    return response;
  }
  if (player_->root()->game_over()) {
    return Response::Error("game is over");
  }

  // TODO(tommadams): Handle out of turn moves.

  Coord c = Coord::kInvalid;
  if (options_.courtesy_pass && player_->root()->move == Coord::kPass) {
    c = Coord::kPass;
  } else {
    if (!options_.tree_reuse) {
      player_->ClearChildren();
    }
    c = player_->SuggestMove(player_->options().num_readouts);
  }
  MG_LOG(INFO) << player_->root()->Describe();
  MG_CHECK(player_->PlayMove(c));

  MaybeStartPondering();

  return Response::Ok(c.ToGtp());
}

GtpClient::Response GtpClient::HandleKnownCommand(CmdArgs args) {
  auto response = CheckArgsExact(1, args);
  if (!response.ok) {
    return response;
  }
  std::string result;
  if (cmd_handlers_.find(std::string(args[0])) != cmd_handlers_.end()) {
    result = "true";
  } else {
    result = "false";
  }
  return Response::Ok(result);
}

GtpClient::Response GtpClient::HandleKomi(CmdArgs args) {
  auto response = CheckArgsExact(1, args);
  if (!response.ok) {
    return response;
  }

  double x;
  if (!absl::SimpleAtod(args[0], &x) || x != game_->options().komi) {
    return Response::Error("unacceptable komi");
  }

  return Response::Ok();
}

GtpClient::Response GtpClient::HandleListCommands(CmdArgs args) {
  auto response = CheckArgsExact(0, args);
  if (!response.ok) {
    return response;
  }
  std::vector<absl::string_view> cmds;
  for (const auto& kv : cmd_handlers_) {
    cmds.push_back(kv.first);
  }
  std::sort(cmds.begin(), cmds.end());

  response.str = absl::StrJoin(cmds, "\n");
  return response;
}

GtpClient::Response GtpClient::HandleLoadsgf(CmdArgs args) {
  auto response = CheckArgsExact(1, args);
  if (!response.ok) {
    return response;
  }

  std::string contents;
  if (!file::ReadFile(std::string(args[0]), &contents)) {
    return Response::Error("cannot load file");
  }

  std::vector<std::unique_ptr<sgf::Node>> trees;
  response = ParseSgf(contents, &trees);
  if (!response.ok) {
    return response;
  }

  NewGame();

  return ReplaySgf(trees);
}

GtpClient::Response GtpClient::HandleName(CmdArgs args) {
  auto response = CheckArgsExact(0, args);
  if (!response.ok) {
    return response;
  }
  return Response::Ok(absl::StrCat("minigo-", player_->model()->name()));
}

GtpClient::Response GtpClient::HandlePlay(CmdArgs args) {
  auto response = CheckArgsExact(2, args);
  if (!response.ok) {
    return response;
  }
  if (player_->root()->game_over()) {
    return Response::Error("game is over");
  }

  Color color;
  if (std::tolower(args[0][0]) == 'b') {
    color = Color::kBlack;
  } else if (std::tolower(args[0][0]) == 'w') {
    color = Color::kWhite;
  } else {
    MG_LOG(ERROR) << "expected b or w for player color, got " << args[0];
    return Response::Error("illegal move");
  }
  if (color != player_->root()->position.to_play()) {
    return Response::Error("out of turn moves are not yet supported");
  }

  Coord c = Coord::FromGtp(args[1], true);
  if (c == Coord::kInvalid) {
    MG_LOG(ERROR) << "expected GTP coord for move, got " << args[1];
    return Response::Error("illegal move");
  }

  if (!player_->PlayMove(c)) {
    return Response::Error("illegal move");
  }

  return Response::Ok();
}

GtpClient::Response GtpClient::HandlePonder(CmdArgs args) {
  auto response = CheckArgsRange(1, 2, args);
  if (!response.ok) {
    return response;
  }

  if (args[0] == "off") {
    // Disable pondering.
    ponder_type_ = PonderType::kOff;
    ponder_read_count_ = 0;
    options_.ponder_limit = 0;
    ponder_duration_ = {};
    ponder_time_limit_ = absl::InfinitePast();
    ponder_limit_reached_ = true;
    return Response::Ok();
  }

  // Subsequent sub commands require exactly 2 arguments.
  response = CheckArgsExact(2, args);
  if (!response.ok) {
    return response;
  }

  if (args[0] == "reads") {
    // Enable pondering limited by number of reads.
    int read_limit;
    if (!absl::SimpleAtoi(args[1], &read_limit) || read_limit <= 0) {
      return Response::Error("couldn't parse read limit");
    }
    options_.ponder_limit = read_limit;
    ponder_type_ = PonderType::kReadLimited;
    ponder_read_count_ = 0;
    ponder_limit_reached_ = false;
    return Response::Ok();
  }

  if (args[0] == "time") {
    // Enable pondering limited by time.
    float duration;
    if (!absl::SimpleAtof(args[1], &duration) || duration <= 0) {
      return Response::Error("couldn't parse time limit");
    }
    ponder_type_ = PonderType::kTimeLimited;
    ponder_duration_ = absl::Seconds(duration);
    ponder_time_limit_ = absl::Now() + ponder_duration_;
    ponder_limit_reached_ = false;
    return Response::Ok();
  }

  return Response::Error("unrecognized ponder mode");
}

GtpClient::Response GtpClient::HandleReadouts(CmdArgs args) {
  auto response = CheckArgsExact(1, args);
  if (!response.ok) {
    return response;
  }

  int x;
  if (!absl::SimpleAtoi(args[0], &x) || x <= 0) {
    return Response::Error("couldn't parse ", args[0], " as an integer > 0");
  } else {
    auto options = player_->options();
    options.num_readouts = x;
    player_->SetOptions(options);
  }

  return Response::Ok();
}

GtpClient::Response GtpClient::HandleShowboard(CmdArgs args) {
  auto response = CheckArgsExact(0, args);
  if (!response.ok) {
    return response;
  }
  return Response::Ok(
      absl::StrCat("\n", player_->root()->position.ToPrettyString(false)));
}

GtpClient::Response GtpClient::HandleUndo(CmdArgs args) {
  auto response = CheckArgsExact(0, args);
  if (!response.ok) {
    return response;
  }

  if (!player_->UndoMove()) {
    return Response::Error("cannot undo");
  }
  if (!options_.tree_reuse) {
    player_->root()->ClearChildren();
  }

  return Response::Ok();
}

GtpClient::Response GtpClient::ParseSgf(
    const std::string& sgf_str,
    std::vector<std::unique_ptr<sgf::Node>>* trees) {
  sgf::Ast ast;
  if (!ast.Parse(sgf_str)) {
    MG_LOG(ERROR) << "couldn't parse SGF";
    return Response::Error("cannot load file");
  }
  if (!sgf::GetTrees(ast, trees)) {
    return Response::Error("cannot load file");
  }
  return Response::Ok();
}

}  // namespace minigo
