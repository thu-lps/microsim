#include "traffic_simulator.h"
#include "pandana_ch/accessibility.h"

namespace LC {

TrafficSimulator::TrafficSimulator(std::shared_ptr<Network> network,
                                   std::shared_ptr<OD> od,
                                   std::shared_ptr<Lanemap> lanemap,
                                   const std::string &save_path) {
  network_ = network;
  od_ = od;
  lanemap_ = lanemap;
  save_path_ = save_path;
  boost::filesystem::path dir(save_path_);
  if (boost::filesystem::create_directory(dir)) {
    std::cout << "Save Dict Directory Created: " << save_path_ << std::endl;
  }
  route_finding_();
}

void TrafficSimulator::route_finding_() {
  // compute routes use contraction hierarchy
  auto graph_ch = std::make_shared<MTC::accessibility::Accessibility>(
      network_->num_vertices(), network_->edge_vertices(),
      network_->edge_weights(), false);

  auto &agents = od_->agents();
  std::vector<long> sources, targets;
  for (const auto &agent : agents) {
    sources.emplace_back(agent.init_intersection);
    targets.emplace_back(agent.end_intersection);
  }

  auto node_sequence = graph_ch->Routes(sources, targets, 0);
  auto &eid2mid = lanemap_->eid2mid();
  //  std::cout << "# of paths = " << all_paths_ch.size() << " \n";

  // add routes to each agent
  for (int i = 0; i < node_sequence.size(); i++) {
    auto &agent = agents[i];
    if (node_sequence[i].size() > 100) {
      std::cerr << "Warning: Agent " << i << " need to go through "
                << node_sequence[i].size() << " edges!" << std::endl;
    }
    if (node_sequence[i].size() == 0) {
      std::cerr << "Warning: Agent " << i << " has no route! " << std::endl;
    } else {
      for (int j = 0; j < node_sequence[i].size() - 1; j++) {
        auto vertex_from = node_sequence[i][j];
        auto vertex_to = node_sequence[i][j + 1];
        auto eid = network_->edge_id(vertex_from, vertex_to);
        auto mid = eid2mid.at(eid);
        agent.route[agent.route_size] = mid;
        agent.route_size++;
      }
    }
  }
}

//
////////////////////////////////////////////////////////
//////// GPU Simulation
////////////////////////////////////////////////////////
void TrafficSimulator::simulateInGPU(float startTime, float endTime,
                                     int save_interval) {

  Benchmarker passesBench("Simulation passes");
  Benchmarker finishCudaBench("Cuda finish");

  Benchmarker microsimulationInGPU("Microsimulation_in_GPU", true);
  microsimulationInGPU.startMeasuring();

  Benchmarker initCudaBench("Init Cuda step");
  Benchmarker simulateBench("Simulation step");
  Benchmarker getDataBench("Data retrieve step");
  Benchmarker shortestPathBench("Shortest path step");
  Benchmarker fileOutput("File_output", true);

  /////////////////////////////////////
  // 1. Init Cuda
  initCudaBench.startMeasuring();
  auto &agents = od_->agents();
  auto &edgesData = lanemap_->edgesData();
  auto &lanemap_data = lanemap_->lanemap_array();
  auto &intersections = lanemap_->intersections();

  std::cout << "Traffic person vec size = " << agents.size() << std::endl;
  std::cout << "EdgesData size = " << edgesData.size() << std::endl;
  std::cout << "LaneMap size = " << lanemap_data.size() << std::endl;
  std::cout << "Intersections size = " << intersections.size() << std::endl;

  init_cuda(true, agents, edgesData, lanemap_data, intersections);

  initCudaBench.stopAndEndBenchmark();

  simulateBench.startMeasuring();
  int numBlocks = ceil(agents.size() / 384.0f);

  std::cout << "Running trafficSimulation with the following configuration:"
            << std::endl
            << ">  Number of people: " << agents.size() << std::endl
            << ">  Number of blocks: " << numBlocks << std::endl
            << ">  Number of threads per block: " << CUDAThreadsPerBlock
            << std::endl;

  std::cerr << "Running main loop from " << (startTime / 3600.0f) << " to "
            << (endTime / 3600.0f) << " with " << agents.size() << "person... "
            << std::endl;

  int num_save_steps = save_interval / deltaTime_;
  unsigned int simulations_steps = 0;
  // 2. Run GPU Simulation
  while (startTime < endTime) {
    cuda_simulate(startTime, agents.size(), intersections.size(), deltaTime_,
                  numBlocks, CUDAThreadsPerBlock);

    simulations_steps += 1;
    startTime += deltaTime_;

    if (simulations_steps % num_save_steps == 0) {
      cuda_get_data(agents, edgesData, intersections); // Get data from cuda
      // Store data to local disk
      save_edges(startTime);
      save_agents(startTime);
    }
  }

  finish_cuda(); // free cuda memory
}

void TrafficSimulator::save_edges(int current_time) {
  std::ofstream file(save_path_ + "edge_data_" + std::to_string(current_time) +
                     ".csv");
  file << "eid"
       << ","
       << "u"
       << ","
       << "v"
       << ","
       << "upstream_count"
       << ","
       << "downstream_count"
       << ","
       << "average_travel_time(s)"
       << "\n";

  const auto &edgesData = lanemap_->edgesData();
  for (const auto &mid_eid : lanemap_->mid2eid()) {
    auto mid = mid_eid.first;
    auto eid = mid_eid.second;

    auto edge_data = edgesData.at(mid);
    float ave_time = -1;
    if (edge_data.downstream_veh_count > 0) {
      ave_time =
          (edge_data.period_cum_travel_steps / edge_data.downstream_veh_count) *
          deltaTime_;
    }

    file << eid << "," << edge_data.vertex[0] << "," << edge_data.vertex[1]
         << "," << edge_data.upstream_veh_count << ","
         << edge_data.downstream_veh_count << "," << ave_time << "\n";
  }
}

void TrafficSimulator::save_agents(int current_time) {
  std::ofstream file(save_path_ + "agents_data_" +
                     std::to_string(current_time) + ".csv");
  file << "aid"
       << ","
       << "ori"
       << ","
       << "dest"
       << ","
       << "type"
       << ","
       << "status"
       << ","
       << "travel_dist(m)"
       << ","
       << "travel_time(s)"
       << ","
       << "ave_speed(m/s)"
       << ","
       << "num_slowdown"
       << ","
       << "num_lane_change"
       << ","
       << "num_in_queue"
       << "\n";
  const auto &agents = od_->agents();
  for (int i = 0; i < agents.size(); ++i) {
    auto agent = agents.at(i);
    file << i << "," << agent.init_intersection << "," << agent.end_intersection
         << "," << agent.agent_type << "," << agent.active << ","
         << agent.cum_length << "," << agent.num_steps * deltaTime_ << ","
         << agent.cum_v / agent.num_steps << "," << agent.slow_down_steps << ","
         << agent.num_lane_change << "," << agent.num_steps_in_queue << "\n";
  }
}

} // namespace LC
