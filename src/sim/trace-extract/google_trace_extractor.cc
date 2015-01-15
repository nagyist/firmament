// The Firmament project
// Copyright (c) 2014 Malte Schwarzkopf <malte.schwarzkopf@cl.cam.ac.uk>
//
// Google cluster trace extractor tool.

#include <cstdio>
#include <string>
#include <vector>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

#include <sys/stat.h>
#include <fcntl.h>

#include "sim/trace-extract/google_trace_extractor.h"
#include "scheduling/dimacs_exporter.h"
#include "scheduling/flow_graph.h"
#include "scheduling/flow_graph_arc.h"
#include "scheduling/flow_graph_node.h"
#include "scheduling/quincy_cost_model.h"
#include "misc/utils.h"
#include "misc/pb_utils.h"
#include "misc/string_utils.h"

using boost::lexical_cast;
using boost::algorithm::is_any_of;
using boost::token_compress_off;

namespace firmament {
namespace sim {

#define MACHINE_TMPL_FILE "../../../tests/testdata/machine_topo.pbin"
//#define MACHINE_TMPL_FILE "/tmp/mach_test.pbin"

DEFINE_int64(num_machines, -1, "Number of machines to extract; -1 for all.");
DEFINE_int64(num_jobs, -1, "Number of initial jobs to extract; -1 for all.");
DEFINE_int64(runtime, -1, "Time to extract data for (from start of trace, in "
             "seconds); -1 for everything.");
DEFINE_string(output_dir, "", "Directory for output flow graphs.");
DEFINE_bool(tasks_preemption_bins, false, "Compute bins of number of preempted tasks.");
DEFINE_int32(bin_time_duration, 10, "Bin size in seconds.");
DEFINE_string(task_bins_output, "bins.out", "The file in which the task bins are written.");
DEFINE_bool(gen_graph_deltas, false, "Generate dimacs delta files. One for each bin.");

GoogleTraceExtractor::GoogleTraceExtractor(string& trace_path)
					: max_machines_(FLAGS_num_machines), max_jobs_(FLAGS_num_jobs),
					  trace_path_(trace_path), machine_parser_(trace_path_),
					  job_parser_(trace_path_), task_parser_(trace_path_) { }

void GoogleTraceExtractor::reset_uuid(
    ResourceTopologyNodeDescriptor* rtnd,
    const string& hostname, const string& root_uuid) {
  string new_uuid;
  if (rtnd->has_parent_id()) {
    // This is an intermediate node, so translate the parent UUID via the lookup
    // table
    const string& old_parent_id = rtnd->parent_id();
    string* new_parent_id = FindOrNull(uuid_conversion_map_, rtnd->parent_id());
    CHECK_NOTNULL(new_parent_id);
    VLOG(2) << "Resetting parent UUID for " << rtnd->resource_desc().uuid()
            << ", parent was " << old_parent_id
            << ", is now " << *new_parent_id;
    rtnd->set_parent_id(*new_parent_id);
    // Grab a new UUID for the node itself
    new_uuid = to_string(GenerateResourceID());
  } else {
    // This is the top of a machine topology, so generate a first UUID for its
    // topology based on its hostname and link it into the root
    rtnd->set_parent_id(root_uuid);
    new_uuid = to_string(GenerateRootResourceID(hostname));
  }
  VLOG(2) << "Resetting UUID for " << rtnd->resource_desc().uuid()
          << " to " << new_uuid;
  InsertOrUpdate(&uuid_conversion_map_, rtnd->resource_desc().uuid(),
                 new_uuid);
  rtnd->mutable_resource_desc()->set_uuid(new_uuid);
}

uint64_t GoogleTraceExtractor::ReadMachinesFile(vector<uint64_t>* machines) {
	int64_t l = 0;
	while (machine_parser_.nextRow()) {
		const MachineEvent &machine = machine_parser_.getMachine();
		if (machine.event_type == MachineEvent::ADD_TYPE) {
			if (machine.timestamp > 0) {
				// we only care about the initial machines here
				break;
			}
			if  (max_machines_ >= 0 && l >= max_machines_) {
				// read as many machines as user requested
				break;
			}
			machines->push_back(machine.machine_id);
			l++;
		}
	}
	return l;
}

uint64_t GoogleTraceExtractor::ReadJobsFile(vector<uint64_t>* jobs) {
	int64_t j = 0;

	JobParser jp(trace_path_);
	while (jp.nextRow()) {
		const JobEvent &job = jp.getJob();
		if (job.event_type == JobEvent::ADD_TYPE) {
			if (job.timestamp > 0) {
				// we only care about the initial machines here
				break;
			}
			if  (max_jobs_ >= 0 && j >= max_jobs_) {
				// read as many machines as user requested
				break;
			}
			jobs->push_back(job.job_id);
			j++;
		}
	}

	return j;
}

uint64_t GoogleTraceExtractor::ReadInitialTasksFile(
    const unordered_map<uint64_t, JobDescriptor*>& jobs) {
	uint64_t t = 0;

	TaskParser tp(trace_path_);
	while (tp.nextRow()) {
		const TaskEvent &task = tp.getTask();

		if (task.event_type == TaskEvent::ADD_TYPE) {
			if (task.timestamp > 0) {
				// we only care about initial tasks
				break;
			}
			if (JobDescriptor* jd = FindPtrOrNull(jobs, task.job_id)) {
				// This is a job we're interested in
				CHECK_NOTNULL(jd);
				TaskDescriptor* root_task = jd->mutable_root_task();
				TaskDescriptor* new_task = root_task->add_spawned();
				new_task->set_uid(GenerateTaskID(*root_task));
				new_task->set_state(TaskDescriptor::RUNNABLE);
				t++;
			}
		}
	}

	return t;
}

void GoogleTraceExtractor::ReplayTrace(FlowGraph* flow_graph, QuincyCostModel* cost_model,
                                       const string& file_base) {
	// no out_file at the moment: we're just reading the trace
}

/*void GoogleTraceExtractor::ReplayTrace(FlowGraph* flow_graph, QuincyCostModel* cost_model,
                                       const string& file_base) {
  char line[200];
  vector<string> vals;
  FILE* fptr = NULL;
  int64_t time_interval_bound = FLAGS_bin_time_duration;
  ofstream out_file(file_base + lexical_cast<string>(0));
  for (uint64_t file_num = 0; file_num < 500; file_num++) {
    string fname;
    spf(&fname, "%s/task_events/part-%05ld-of-00500.csv", trace_path_.c_str(), file_num);
    if ((fptr = fopen(fname.c_str(), "r")) == NULL) {
      LOG(ERROR) << "Failed to open trace for reading of task events.";
    }
    while (!feof(fptr)) {
      if (fscanf(fptr, "%[^\n]%*[\n]", &line[0]) > 0) {
        boost::split(vals, line, is_any_of(","), token_compress_off);
        if (vals.size() != 13) {
          LOG(ERROR) << "Unexpected structure of task event row: found "
                     << vals.size() << " columns.";
        } else {
          int64_t task_time = lexical_cast<uint64_t>(vals[0]);
          uint64_t job_id = lexical_cast<uint64_t>(vals[2]);
          // TODO(ionel): The task_id should be a unique task id. However, this is not a problem
          // if we use the random cost model.
          uint64_t task_id = lexical_cast<uint64_t>(vals[3]);
          uint64_t event_type = lexical_cast<uint64_t>(vals[5]);
          if (task_time == 0) {
            continue;
          }
          if (FLAGS_runtime < task_time) {
            LOG(INFO) << "Terminating at : " << task_time;
            out_file.close();
            return;
          }
          if (event_type == 0) {
            if (task_time <= time_interval_bound) {
              AddNewTask(flow_graph, cost_model, job_id, task_id, out_file);
            } else {
              out_file.close();
              time_interval_bound += FLAGS_bin_time_duration;
              while (time_interval_bound < task_time) {
                time_interval_bound += FLAGS_bin_time_duration;
              }
              out_file.open(file_base + lexical_cast<string>(time_interval_bound));
              AddNewTask(flow_graph, cost_model, job_id, task_id, out_file);
            }
          }
        }
      }
    }
  }
  out_file.close();
}*/

void GoogleTraceExtractor::AddNewTask(FlowGraph* flow_graph, QuincyCostModel* cost_model,
                                      uint64_t job_id, uint64_t task_id, ofstream& out_file) {
  JobID_t* jdp = FindOrNull(job_id_conversion_map_, job_id);
  if (!jdp) {
    // Add new job to the graph
    JobDescriptor* jd = new JobDescriptor();
    PopulateJob(jd, job_id);
    flow_graph->AddJobNodes(jd);
    // internal job ID is updated by PopulateJob
    jdp = FindOrNull(job_id_conversion_map_, job_id);
    CHECK_NOTNULL(jdp);
    out_file << "d 0 0 1\n";
    out_file << "a 0 2 0 1 0\n";
  }
  FlowGraphNode* unsched_agg = flow_graph->GetUnschedAggForJob(*jdp);
  out_file << "d 1 0 2\n";
  out_file << "a 0 2 0 1 " << cost_model->TaskToClusterAggCost(task_id) << "\n";
  out_file << "a 0 " << unsched_agg->id_ << " 0 1 " <<
    cost_model->TaskToUnscheduledAggCost(task_id) << "\n";
  FlowGraphArc** arc_to_sink = FindOrNull(unsched_agg->outgoing_arc_map_, 1);
  CHECK_NOTNULL(arc_to_sink);
  CHECK_NOTNULL(*arc_to_sink);
  out_file << "x " << unsched_agg->id_ << " 1 " << (*arc_to_sink)->cap_lower_bound_ << " " <<
    (++(*arc_to_sink)->cap_upper_bound_) << " " << (*arc_to_sink)->cost_ << "\n";
}

void GoogleTraceExtractor::BinTasks(ofstream& out_file) {
  char line[200];
  vector<string> vals;
  FILE* fptr = NULL;
  uint64_t time_interval_bound = FLAGS_bin_time_duration;
  uint64_t num_preempted_tasks = 0;
  for (uint64_t file_num = 0; file_num < 500; file_num++) {
    string fname;
    spf(&fname, "%s/task_events/part-%05ld-of-00500.csv", trace_path_.c_str(), file_num);
    if ((fptr = fopen(fname.c_str(), "r")) == NULL) {
      LOG(ERROR) << "Failed to open trace for reading of task events.";
    }
    while (!feof(fptr)) {
      if (fscanf(fptr, "%[^\n]%*[\n]", &line[0]) > 0) {
        boost::split(vals, line, is_any_of(","), token_compress_off);
        if (vals.size() != 13) {
          LOG(ERROR) << "Unexpected structure of task event row: found "
                     << vals.size() << " columns.";
        } else {
          uint64_t task_time = lexical_cast<uint64_t>(vals[0]);
          uint64_t event_type = lexical_cast<uint64_t>(vals[5]);
          if (event_type == 2) {
            if (task_time <= time_interval_bound) {
              num_preempted_tasks++;
            } else {
              out_file << "(" << time_interval_bound - FLAGS_bin_time_duration << ", " <<
                time_interval_bound << "]: " << num_preempted_tasks << "\n";
              time_interval_bound += FLAGS_bin_time_duration;
              while (time_interval_bound < task_time) {
                out_file << "(" << time_interval_bound - FLAGS_bin_time_duration << ", " <<
                  time_interval_bound << "]: 0\n";
                time_interval_bound += FLAGS_bin_time_duration;
              }
              num_preempted_tasks = 1;
            }
          }
        }
      }
    }
  }
  out_file << "(" << time_interval_bound - FLAGS_bin_time_duration << ", " <<
    time_interval_bound << "]: " << num_preempted_tasks << "\n";
}

void GoogleTraceExtractor::PopulateJob(JobDescriptor* jd, uint64_t job_id) {
  // XXX(malte): job_id argument is discareded and replaced by randomly
  // generated ID at the moment.
  JobID_t new_job_id = GenerateJobID();
  InsertOrUpdate(&job_id_conversion_map_, job_id, new_job_id);
  jd->set_uuid(to_string(new_job_id));
  TaskDescriptor* rt = jd->mutable_root_task();
  string bin;
  // XXX(malte): hack, should use logical job name
  spf(&bin, "%jd", job_id);
  rt->set_binary(bin);
  rt->set_uid(GenerateRootTaskID(*jd));
  rt->set_state(TaskDescriptor::RUNNABLE);
}

void GoogleTraceExtractor::AddMachineToTopology(
    const ResourceTopologyNodeDescriptor& machine_tmpl,
    uint64_t machine_id,
    ResourceTopologyNodeDescriptor &rtn_root) {
  ResourceTopologyNodeDescriptor* child = rtn_root.add_children();
  child->CopyFrom(machine_tmpl);
  const string& root_uuid = rtn_root.resource_desc().uuid();
  char hn[100];
  sprintf(hn, "h%ju", machine_id);
  TraverseResourceProtobufTreeReturnRTND(
      child, boost::bind(&GoogleTraceExtractor::reset_uuid, this, _1,
                         string(hn), root_uuid));
  child->mutable_resource_desc()->set_friendly_name(hn);
}

ResourceTopologyNodeDescriptor&
GoogleTraceExtractor::LoadInitialTopology() {
  // Import a fictional machine resource topology
  int fd = open(MACHINE_TMPL_FILE, O_RDONLY);
  machine_tmpl.ParseFromFileDescriptor(fd);
  close(fd);

  // Create the root node
  ResourceTopologyNodeDescriptor* rtn_root = new
      ResourceTopologyNodeDescriptor();
  ResourceID_t root_uuid = GenerateRootResourceID("XXXgoogleXXX");
  rtn_root->mutable_resource_desc()->set_uuid(to_string(root_uuid));
  LOG(INFO) << "Root res ID is " << to_string(root_uuid);
  InsertIfNotPresent(&uuid_conversion_map_, to_string(root_uuid),
                     to_string(root_uuid));

  return *rtn_root;
}

void GoogleTraceExtractor::LoadInitialMachines(
										ResourceTopologyNodeDescriptor &root) {
	vector<uint64_t> machines;
	// Read the initial machine events from trace
	uint64_t num_machines = ReadMachinesFile(&machines);
	LOG(INFO) << "Loaded " << num_machines << " machines!";

	// Create each machine and add it to the graph
	uint64_t i = 0;
	for (vector<uint64_t>::const_iterator iter = machines.begin();
	   iter != machines.end();
	   ++iter) {
		AddMachineToTopology(machine_tmpl, i, root);
		++i;
	}
	LOG(INFO) << "Added " << machines.size() << " machines.";
}

unordered_map<uint64_t, JobDescriptor*>& GoogleTraceExtractor::LoadInitialJobs() {
  vector<uint64_t> job_ids;
  unordered_map<uint64_t, JobDescriptor*>* jobs;

  // Read the initial machine events from trace
  uint64_t num_jobs_read = ReadJobsFile(&job_ids);
  LOG(INFO) << "Read " << num_jobs_read << " initial jobs.";
  jobs = new unordered_map<uint64_t, JobDescriptor*>();

  // Add jobs
  for (vector<uint64_t>::const_iterator iter = job_ids.begin();
       iter != job_ids.end();
       ++iter) {
    JobDescriptor* jd = new JobDescriptor();
    PopulateJob(jd, *iter);
    CHECK(InsertOrUpdate(jobs, *iter, jd));
  }
  return *jobs;
}

void GoogleTraceExtractor::LoadInitalTasks(
    const unordered_map<uint64_t, JobDescriptor*>& initial_jobs) {
  // Read initial tasks from trace
  // and add tasks to jobs in initial_jobs
  uint64_t num_tasks = ReadInitialTasksFile(initial_jobs);
  LOG(INFO) << "Added " << num_tasks << " initial tasks to "
            << initial_jobs.size() << " initial jobs.";
}

void GoogleTraceExtractor::Run() {
  LOG(INFO) << "Starting Google Trace extraction!";
  LOG(INFO) << "Number of machines to extract: " << FLAGS_num_machines;
  LOG(INFO) << "Time to extract for: " << FLAGS_runtime << " seconds.";

  ResourceTopologyNodeDescriptor& initial_resource_topology =
      LoadInitialTopology();
  unordered_map<uint64_t, JobDescriptor*>& initial_jobs =
      LoadInitialJobs();
  LoadInitalTasks(initial_jobs);

  QuincyCostModel* cost_model = new QuincyCostModel();
  FlowGraph g(cost_model);
  // Add resources and job to flow graph
  g.AddResourceTopology(initial_resource_topology);
  // Add initial jobs
  for (unordered_map<uint64_t, JobDescriptor*>::const_iterator
       iter = initial_jobs.begin();
       iter != initial_jobs.end();
       ++iter) {
    VLOG(1) << "Add job with " << iter->second->root_task().spawned_size()
            << " child tasks of root task";
    g.AddJobNodes(iter->second);
  }

  // Export initial graph
  DIMACSExporter exp;
  exp.Export(g);
  string outname = FLAGS_output_dir + "/test.dm";
  VLOG(1) << "Output written to " << outname;
  exp.Flush(outname);
  if (FLAGS_tasks_preemption_bins) {
    ofstream out_file(FLAGS_output_dir + "/" + FLAGS_task_bins_output);
    if (out_file.is_open()) {
      BinTasks(out_file);
      out_file.close();
    } else {
      LOG(ERROR) << "Could not open bin output file.";
    }
  }
  if (FLAGS_gen_graph_deltas) {
    ReplayTrace(&g, cost_model, FLAGS_output_dir + "/delta_");
  }
}

}  // namespace sim
}  // namespace firmament