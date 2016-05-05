//---------------------------------------------------------
// Copyright 2016 Ontario Institute for Cancer Research
// Written by Jared Simpson (jared.simpson@oicr.on.ca)
//---------------------------------------------------------
//
// nanopolish_trainmodel - train a new pore model from
// the FAST5 output of a basecaller
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <inttypes.h>
#include <assert.h>
#include <math.h>
#include <sys/time.h>
#include <algorithm>
#include <sstream>
#include <set>
#include <omp.h>
#include <getopt.h>
#include "htslib/faidx.h"
#include "nanopolish_poremodel.h"
#include "nanopolish_squiggle_read.h"
#include "nanopolish_methyltrain.h"
#include "training_core.hpp"
#include "profiler.h"

//
// Typedefs
//
typedef std::vector<StateTrainingData> TrainingDataVector;
typedef std::vector<TrainingDataVector> KmerTrainingData;

//
// Getopt
//
#define SUBPROGRAM "trainmodel"

static const char *TRAINMODEL_VERSION_MESSAGE =
SUBPROGRAM " Version " PACKAGE_VERSION "\n"
"Written by Jared Simpson.\n"
"\n"
"Copyright 2016 Ontario Institute for Cancer Research\n";

static const char *TRAINMODEL_USAGE_MESSAGE =
"Usage: " PACKAGE_NAME " " SUBPROGRAM " [OPTIONS] input.fofn\n"
"Train a new pore model using the basecalled reads in input.fofn\n"
"\n"
"  -v, --verbose                        display verbose output\n"
"      --version                        display version\n"
"      --help                           display this help and exit\n"
"\nReport bugs to " PACKAGE_BUGREPORT "\n\n";

namespace opt
{
    static unsigned int verbose;
    static std::string fofn_file;
}

static const char* shortopts = "v";

enum { OPT_HELP = 1, OPT_VERSION };

static const struct option longopts[] = {
    { "verbose",     no_argument,       NULL, 'v' },
    { "help",        no_argument,       NULL, OPT_HELP },
    { "version",     no_argument,       NULL, OPT_VERSION },
    { NULL, 0, NULL, 0 }
};

void parse_trainmodel_options(int argc, char** argv)
{
    bool die = false;
    for (char c; (c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1;) {
        std::istringstream arg(optarg != NULL ? optarg : "");
        switch (c) {
            case '?': die = true; break;
            case 'v': opt::verbose++; break;
            case OPT_HELP:
                std::cout << TRAINMODEL_USAGE_MESSAGE;
                exit(EXIT_SUCCESS);
            case OPT_VERSION:
                std::cout << TRAINMODEL_VERSION_MESSAGE;
                exit(EXIT_SUCCESS);
        }
    }
    
    if (argc - optind < 1) {
        std::cerr << SUBPROGRAM ": not enough arguments\n";
        die = true;
    }

    if (argc - optind > 1) {
        std::cerr << SUBPROGRAM ": too many arguments\n";
        die = true;
    }

    if (die) 
    {
        std::cout << "\n" << TRAINMODEL_USAGE_MESSAGE;
        exit(EXIT_FAILURE);
    }

    opt::fofn_file = argv[optind++];
}

std::vector<EventAlignment> generate_alignment_to_basecalls(const SquiggleRead* read, 
                                                            const size_t k,
                                                            const size_t strand_idx,
                                                            const std::vector<bool>* use_kmer = NULL)
{
    size_t num_kmers_in_alphabet = gDNAAlphabet.get_num_strings(k);
    std::vector<EventAlignment> alignment;
    const std::string& read_sequence = read->read_sequence;
    size_t n_kmers = read_sequence.size() - k + 1;
    for(size_t ki = 0; ki < n_kmers; ++ki) {

        IndexPair event_range_for_kmer = read->base_to_event_map[ki].indices[strand_idx];
        
        // skip kmers without events and with multiple events
        if(event_range_for_kmer.start == -1 || 
           event_range_for_kmer.start != event_range_for_kmer.stop) {
            continue;
        }

        std::string kmer = read_sequence.substr(ki, k);
        size_t kmer_rank = gDNAAlphabet.kmer_rank(kmer.c_str(), k);
        assert(kmer_rank < num_kmers_in_alphabet);

        size_t event_idx = event_range_for_kmer.start;
        
        // Check if this kmer is marked as being useful
        if(use_kmer == NULL || use_kmer->at(kmer_rank)) {
            EventAlignment ea;
            // ref data
            ea.ref_name = ""; // not needed
            ea.ref_kmer = kmer;
            ea.ref_position = ki;
            ea.read_idx = -1; // not needed
            ea.strand_idx = strand_idx;
            ea.event_idx = event_idx;
            ea.rc = false;
            ea.model_kmer = kmer;
            ea.hmm_state = 'M'; // recalibration code only uses "M" alignments
            alignment.push_back(ea);
        }
    }

    return alignment;
}

KmerTrainingData alignment_to_training_data(const SquiggleRead* read,
                                            const std::vector<EventAlignment>& alignment,
                                            const size_t k,
                                            size_t read_idx)
{
    size_t num_kmers_in_alphabet = gDNAAlphabet.get_num_strings(k);
    KmerTrainingData kmer_training_data(num_kmers_in_alphabet);

    for(auto const& a : alignment) {
        size_t kmer_rank = gDNAAlphabet.kmer_rank(a.model_kmer.c_str(), k);
        assert(kmer_rank < num_kmers_in_alphabet);

        double level = read->events[a.strand_idx][a.event_idx].mean;
        double stdv = read->events[a.strand_idx][a.event_idx].stdv;

        StateTrainingData std(level, stdv, read->pore_model[a.strand_idx].var);
        kmer_training_data[kmer_rank].push_back(std);
        
        //fprintf(tsv_writer, "%zu\t%s\t%.2lf\t%.5lf\n", read_idx, kmer.c_str(), level, read->events[training_strand][event_idx].duration);
    }
    return kmer_training_data;
}


int trainmodel_main(int argc, char** argv)
{
    parse_trainmodel_options(argc, argv);

    std::ifstream fofn_reader(opt::fofn_file);
    std::string fast5_name;

    // Read input
    std::vector<SquiggleRead*> reads;
    while(getline(fofn_reader, fast5_name)) {
        fprintf(stderr, "Loading %s\n", fast5_name.c_str());
        reads.push_back(new SquiggleRead(fast5_name, fast5_name));
    }
    fprintf(stderr, "Loaded %zu reads\n", reads.size());

    // 
    unsigned int basecalled_k = 5; // TODO: infer this
    size_t num_kmers = gDNAAlphabet.get_num_strings(basecalled_k);
    unsigned int training_strand = T_IDX; // template training for now

    // This vector is indexed by read, then kmer, then event
    std::vector<KmerTrainingData> read_training_data;

    FILE* tsv_writer = fopen("trainmodel.tsv", "w");
    fprintf(tsv_writer, "read_idx\tkmer\tlevel_mean\tduration\n");

    size_t read_idx = 0;
    for(auto* read : reads) {
        
        // extract alignment of events to k-mers
        std::vector<EventAlignment> alignment = 
            generate_alignment_to_basecalls(read, 
                                            basecalled_k,
                                            training_strand,
                                            NULL);


        // convert the alignment into model training data for this read
        KmerTrainingData training_data = 
            alignment_to_training_data(read, 
                                       alignment,
                                       basecalled_k,
                                       read_idx);

        read_training_data.push_back(training_data);
        read_idx++;
    }

    // Select the read with the most events as the "baseline" read for generating the model
    size_t max_events = 0;
    size_t max_events_index = 0;
    for(size_t rti = 0; rti < read_training_data.size(); ++rti) {
        auto& kmer_training_data = read_training_data[rti];
        size_t total_events = 0;

        for(size_t ki = 0; ki < kmer_training_data.size(); ++ki) {
            total_events += kmer_training_data[ki].size();
        }
        printf("read %zu has %zu events (max: %zu, %zu)\n", rti, total_events, max_events, max_events_index);

        if(total_events > max_events) {
            max_events = total_events;
            max_events_index = rti;
        }
    }

    // Set the initial pore model based on the read with the most events
    PoreModel pore_model(basecalled_k);
    pore_model.states.resize(num_kmers);
    pore_model.scaled_states.resize(num_kmers);
    pore_model.scaled_params.resize(num_kmers);

    pore_model.shift = 0.0;
    pore_model.scale = 1.0;
    pore_model.drift = 0.0;
    pore_model.var = 1.0;
    pore_model.scale_sd = 1.0;
    pore_model.var_sd = 1.0;
    
    auto& kmer_training_data_for_selected = read_training_data[max_events_index];

    std::vector<bool> use_kmer(num_kmers, false);
    for(size_t ki = 0; ki < kmer_training_data_for_selected.size(); ++ki) {
        std::vector<double> values;
        std::stringstream ss;
        for(size_t ei = 0; ei < kmer_training_data_for_selected[ki].size(); ++ei) {
            values.push_back(kmer_training_data_for_selected[ki][ei].level_mean);
            ss << values.back() << " ";
        }

        // Set the kmer's mean parameter to be the median of the recorded values
        std::sort(values.begin(), values.end());

        size_t n = values.size();
        double median;
        if(n == 0) {
            median = 0.0f;
        } else {
            if(n % 2 == 0) {
                median = (values[n / 2 - 1] + values[n/2]) / 2.0f;
            } else {
                median = values[n/2];
            }

            // mark this kmer as valid
            use_kmer[ki] = true;
            pore_model.states[ki].level_mean = median;
            pore_model.states[ki].level_stdv = 1.0;
            printf("k: %zu median: %.2lf values: %s\n", ki, median, ss.str().c_str());
        }
    }
    pore_model.bake_gaussian_parameters();

    // Apply model to each read
    for(auto* read: reads) {
        read->pore_model[training_strand] = pore_model;
    }

    // Recalibrate read
    for(auto* read: reads) {

        // generate an alignment between the RNN output and the basecalled read
        std::vector<EventAlignment> alignment = 
            generate_alignment_to_basecalls(read, 
                                            basecalled_k,
                                            training_strand,
                                            &use_kmer);

        // recalibrate shift/scale/etc
        recalibrate_model(*read, 
                          training_strand,
                          alignment,
                          &gDNAAlphabet,
                          false);
        
        const PoreModel& read_model = read->pore_model[training_strand];
        printf("[recalibration] events: %zu alignment: %zu shift: %.2lf scale: %.2lf drift: %.4lf var: %.2lf\n", 
            read->events[training_strand].size(),
            alignment.size(),
            read_model.shift, 
            read_model.scale, 
            read_model.drift, 
            read_model.var);

    }
    
    // Deallocate input reads
    for(auto* read : reads) {
        delete read;
    }

    return 0;
}