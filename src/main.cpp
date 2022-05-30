#include "vcf.h"
#include "print.h"
#include "globals.h"
#include "fasta.h"
#include "bed.h"
#include "dist.h"
#include "cluster.h"
#include "phase.h"

Globals g;
std::vector<std::string> type_strs = { "REF", "SUB", "INS", "DEL", "GRP"};
std::vector<std::string> gt_strs = {
".|.", "0|0", "0|1", "0|2", "1|0", "1|1", "1|2", "2|0", "2|1", "2|2", "?|?" };
std::vector<std::string> region_strs = {"OUTSIDE  ", "INSIDE   ", "BORDER   ", "OTHER CTG"};
std::vector<std::string> aln_strs = {"CAL1-HAP1", "CAL1-HAP2", "CAL2-HAP1", "CAL2-HAP2"};
std::vector<std::string> phase_strs = {"0", "1", "-"};
 
int main(int argc, char **argv) {

    // parse and store command-line args
    g.parse_args(argc, argv);
    fastaData ref(g.ref_fasta_fp);

    INFO(" ");
    INFO("Parsing Calls VCF '%s'", g.calls_vcf_fn.data());
    vcfData calls(g.calls_vcf_fp);
    /* edit_dist_realign(&calls, &ref); */

    INFO(" ");
    INFO("Parsing Truth VCF '%s'", g.truth_vcf_fn.data());
    vcfData truth(g.truth_vcf_fp);
    /* edit_dist_realign(&truth, &ref); */

    // save per-cluster alignment info
    clusterData clusters = edit_dist(&calls, &truth, &ref);

    // phase clusters
    phaseData phasings(&clusters);

    return EXIT_SUCCESS;
}
