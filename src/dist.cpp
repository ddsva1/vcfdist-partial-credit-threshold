#include <string>
#include <vector>
#include <cstdio>
#include <chrono>
#include <utility>
#include <queue>

#include "dist.h"
#include "print.h"
#include "cluster.h"


vcfData edit_dist_realign(const vcfData* vcf, const fastaData* const ref) {

    // copy vcf header data over to results vcf
    vcfData results;
    results.sample = vcf->sample;
    results.contigs = vcf->contigs;
    results.lengths = vcf->lengths;
    results.ref = vcf->ref;
    for (auto ctg : results.contigs) 
        for (int hap = 0; hap < 2; hap++) 
            results.hapcalls[hap][ctg] = variantCalls();

    // iterate over each haplotype
    int clusters = 0;
    int new_ed_clusters = 0;
    int old_ed = 0;
    int new_ed = 0;
    for (int h = 0; h < 2; h++) {

        // iterate over each contig
        for (auto itr = vcf->hapcalls[h].begin(); 
                itr != vcf->hapcalls[h].end(); itr++) {
            std::string ctg = itr->first;
            variantCalls vars = itr->second;
            if (vars.poss.size() == 0) continue;

            // iterate over each cluster of variants
            for (size_t var_grp = 0; var_grp < vars.clusters.size()-1; var_grp++) {
                int beg_idx = vars.clusters[var_grp];
                int end_idx = vars.clusters[var_grp+1];
                int beg = vars.poss[beg_idx]-1;
                int end = vars.poss[end_idx-1] + vars.rlens[end_idx-1]+1;
                int subs = 0;
                int inss = 0;
                int dels = 0;

                // iterate over variants, summing edit distance
                for (int var = beg_idx; var < end_idx; var++) {
                    switch (vars.types[var]) {
                        case TYPE_SUB: subs++; break;
                        case TYPE_INS: inss += vars.alts[var].size(); break;
                        case TYPE_DEL: dels += vars.refs[var].size(); break;
                        default: ERROR("unexpected variant type (%i)", vars.types[var]) 
                                 std::exit(1); break;
                    }
                }

                // colored alignment
                int var = beg_idx;
                std::string ref_out_str = "";
                std::string alt_out_str = "";
                std::string alt_str = "";
                for (int ref_pos = beg; ref_pos < end;) {
                    if (ref_pos == vars.poss[var]) { // in variant
                        switch (vars.types[var]) {
                            case TYPE_INS:
                                alt_str += vars.alts[var];
                                alt_out_str += GREEN(vars.alts[var]);
                                ref_out_str += std::string(vars.alts[var].size(), ' ');
                                break;
                            case TYPE_DEL:
                                alt_out_str += std::string(vars.refs[var].size(), ' ');
                                ref_out_str += RED(vars.refs[var]);
                                ref_pos += vars.refs[var].size();
                                break;
                            case TYPE_SUB:
                                alt_str += vars.alts[var];
                                alt_out_str += " " + GREEN(vars.alts[var]);
                                ref_out_str += RED(vars.refs[var]) + " ";
                                ref_pos++;
                                break;
                            case TYPE_GRP:
                                alt_str += vars.alts[var];
                                alt_out_str += std::string(vars.refs[var].size(), ' ') 
                                    + GREEN(vars.alts[var]);
                                ref_out_str += RED(vars.refs[var]) + std::string(
                                        vars.alts[var].size(), ' ');
                                ref_pos += vars.refs[var].size();
                                break;
                        }
                        var++; // next variant
                    }
                    else { // match
                        try {
                            alt_str += ref->fasta.at(ctg)[ref_pos];
                            ref_out_str += ref->fasta.at(ctg)[ref_pos];
                            alt_out_str += ref->fasta.at(ctg)[ref_pos];
                            ref_pos++;
                        } catch (const std::out_of_range & e) {
                            ERROR("contig %s not present in reference FASTA",
                                    ctg.data());
                            exit(1);
                        }
                    }
                }

                // do alignment
                std::string ref_str = ref->fasta.at(ctg).substr(beg, end-beg);
                int ref_len = ref_str.size();
                int alt_len = alt_str.size();
                int curr_score = 0;
                std::vector< std::vector<int> > ptrs(alt_len, std::vector<int>(ref_len));
                std::vector< std::vector<bool> > done(alt_len, std::vector<bool>(ref_len));
                std::queue< std::pair<int,int> > curr_q, next_q;

                // init
                ptrs[0][0] = PTR_DIAG;
                done[0][0] = true;

                // set first diag (wavefront)
                curr_q.push({0,0});
                for(int n = 1; n < std::min(ref_len, alt_len); n++) {
                    if (ref_str[n] == alt_str[n]) {
                        curr_q.push({n,n});
                        ptrs[n][n] = PTR_DIAG;
                        done[n][n] = true;
                    } else  {
                        break;
                    }
                }

                // continue alignment waves until fully aligned
                while (!done[alt_len-1][ref_len-1]) {

                    // expand to next wavefront
                    while (!curr_q.empty()) {

                        // get current cell
                        std::pair<int,int> cell = curr_q.front(); curr_q.pop();
                        int alt_idx = cell.first;
                        int ref_idx = cell.second;

                        // expand down, then diagonally
                        if (alt_idx+1 < alt_len && !done[alt_idx+1][ref_idx]) {
                            next_q.push({alt_idx+1, ref_idx});
                            ptrs[alt_idx+1][ref_idx] |= PTR_UP;
                            int off = 1;
                            while (alt_idx+1+off < alt_len && 
                                    ref_idx+off < ref_len && 
                                    !done[alt_idx+1+off][ref_idx+off] &&
                                    alt_str[alt_idx+1+off] == ref_str[ref_idx+off]) {
                                next_q.push({alt_idx+1+off, ref_idx+off});
                                ptrs[alt_idx+1+off][ref_idx+off] |= PTR_DIAG;
                                off++;
                            }
                        }

                        // expand right, then diagonally
                        if (ref_idx+1 < ref_len && !done[alt_idx][ref_idx+1]) {
                            next_q.push({alt_idx, ref_idx+1});
                            ptrs[alt_idx][ref_idx+1] |= PTR_LEFT;
                            int off = 1;
                            while (alt_idx+off < alt_len && 
                                    ref_idx+1+off < ref_len && 
                                    !done[alt_idx+off][ref_idx+1+off] &&
                                    alt_str[alt_idx+off] == ref_str[ref_idx+1+off]) {
                                next_q.push({alt_idx+off, ref_idx+1+off});
                                ptrs[alt_idx+off][ref_idx+1+off] |= PTR_DIAG;
                                off++;
                            }
                        }
                    }

                    // current queue empty, transfer next over
                    while (!next_q.empty()) {

                        // get current cell
                        std::pair<int,int> cell = next_q.front(); next_q.pop();
                        int alt_idx = cell.first;
                        int ref_idx = cell.second;

                        if (!done[alt_idx][ref_idx]) {
                            // add to current wavefront, if new
                            curr_q.push(cell);

                            // mark cell as traversed
                            done[alt_idx][ref_idx] = true;
                        }
                    }
                    curr_score++;
                }

                // left backtrack (prioritize LEFT)
                int alt_idx = alt_len-1;
                int ref_idx = ref_len-1;
                std::vector< std::pair<int,int> > left_path;
                while (alt_idx >= 0 || ref_idx >= 0) {
                    left_path.push_back({alt_idx, ref_idx});
                    ptrs[alt_idx][ref_idx] |= LEFT_PATH;
                    if (ptrs[alt_idx][ref_idx] & PTR_LEFT) {
                        ref_idx--;
                    } else if (ptrs[alt_idx][ref_idx] & PTR_DIAG) {
                        ref_idx--; alt_idx--;
                    } else if (ptrs[alt_idx][ref_idx] & PTR_UP) {
                        alt_idx--;
                    } else {
                        ERROR("no pointer during left backtrack at (%d,%d)", alt_idx, ref_idx);
                    }
                }
                std::reverse(left_path.begin(), left_path.end());

                // right backtrack (prioritize UP)
                alt_idx = alt_len-1;
                ref_idx = ref_len-1;
                std::vector< std::pair<int,int> > right_path;
                while (alt_idx >= 0 || ref_idx >= 0) {
                    right_path.push_back({alt_idx, ref_idx});
                    ptrs[alt_idx][ref_idx] |= RIGHT_PATH;
                    if (ptrs[alt_idx][ref_idx] & PTR_UP) {
                        alt_idx--;
                    } else if (ptrs[alt_idx][ref_idx] & PTR_DIAG) {
                        ref_idx--; alt_idx--;
                    } else if (ptrs[alt_idx][ref_idx] & PTR_LEFT) {
                        ref_idx--;
                    } else {
                        ERROR("no pointer during right backtrack at (%d,%d)", alt_idx, ref_idx);
                    }
                }
                std::reverse(right_path.begin(), right_path.end());
                

                // DEBUG PRINT
                if (g.print_verbosity >= 2) {

                    // create array
                    std::vector< std::vector<char> > ptr_str;
                    for (int i = 0; i < alt_len*2; i++)
                        ptr_str.push_back(std::vector<char>(ref_len*2, ' '));

                    // set arrows
                    for (int alt_idx = 0; alt_idx < alt_len; alt_idx++) {
                        for(int ref_idx = 0; ref_idx < ref_len; ref_idx++) {
                            ptr_str[alt_idx*2+1][ref_idx*2+1] = '*';
                            if (ptrs[alt_idx][ref_idx] & PTR_DIAG)
                                ptr_str[alt_idx*2][ref_idx*2] = '\\';
                            if (ptrs[alt_idx][ref_idx] & PTR_LEFT)
                                ptr_str[alt_idx*2+1][ref_idx*2] = '-';
                            if (ptrs[alt_idx][ref_idx] & PTR_UP)
                                ptr_str[alt_idx*2][ref_idx*2+1] = '|';
                        }
                    }

                    // print array
                    for (int i = -1; i < alt_len*2; i++) {
                        for (int j = -1; j < ref_len*2; j++) {
                            if (i < 0 && j < 0) {
                                printf("\n  ");
                            }
                            else if (i < 0) {
                                if (!(j%2)) printf("%c ", ref_str[j>>1]);
                            } else if (j < 0) {
                                if (!(i%2)) printf("%c ", alt_str[i>>1]);
                                else printf("  ");
                            } else {
                                switch(ptr_str[i][j]) {
                                    case '*':
                                        if (ptrs[i>>1][j>>1] & LEFT_PATH) {
                                            if (ptrs[i>>1][j>>1] & RIGHT_PATH) { // both
                                                printf("%s", GREEN('*').data());
                                            } else { // left
                                                printf("%s", YELLOW('*').data());
                                            }
                                        } else if (ptrs[i>>1][j>>1] & RIGHT_PATH) { // right
                                            printf("%s", BLUE('*').data());
                                        } else { // none
                                            printf("*");
                                        }
                                        break;
                                    case '\\':
                                        if (ptrs[i>>1][j>>1] & LEFT_PATH && 
                                                (i>>1) > 0 && (j>>1) > 0 &&
                                                ptrs[(i>>1)-1][(j>>1)-1] & LEFT_PATH) {
                                            if (ptrs[i>>1][j>>1] & RIGHT_PATH &&
                                                    ptrs[(i>>1)-1][(j>>1)-1] & RIGHT_PATH) { // both
                                                printf("%s", GREEN('\\').data());
                                            } else { // left
                                                printf("%s", YELLOW('\\').data());
                                            }
                                        } else if (ptrs[i>>1][j>>1] & RIGHT_PATH &&
                                                (i>>1) > 0 && (j>>1) > 0 &&
                                                ptrs[(i>>1)-1][(j>>1)-1] & RIGHT_PATH) { // right
                                            printf("%s", BLUE('\\').data());
                                        } else { // none
                                            printf("\\");
                                        }
                                        break;
                                    case '-':
                                        if (ptrs[i>>1][j>>1] & LEFT_PATH && 
                                                (j>>1) > 0 &&
                                                ptrs[i>>1][(j>>1)-1] & LEFT_PATH) {
                                            if (ptrs[i>>1][j>>1] & RIGHT_PATH &&
                                                    ptrs[i>>1][(j>>1)-1] & RIGHT_PATH) { // both
                                                printf("%s", GREEN('-').data());
                                            } else { // left
                                                printf("%s", YELLOW('-').data());
                                            }
                                        } else if (ptrs[i>>1][j>>1] & RIGHT_PATH && 
                                                (j>>1) > 0 &&
                                                ptrs[i>>1][(j>>1)-1] & RIGHT_PATH) { // right
                                            printf("%s", BLUE('-').data());
                                        } else { // none
                                            printf("-");
                                        }
                                        break;
                                    case '|':
                                        if (ptrs[i>>1][j>>1] & LEFT_PATH && 
                                                (i>>1) > 0 &&
                                                ptrs[(i>>1)-1][j>>1] & LEFT_PATH) {
                                            if (ptrs[i>>1][j>>1] & RIGHT_PATH &&
                                                    ptrs[(i>>1)-1][j>>1] & RIGHT_PATH) { // both
                                                printf("%s", GREEN('|').data());
                                            } else { // left
                                                printf("%s", YELLOW('|').data());
                                            }
                                        } else if (ptrs[i>>1][j>>1] & RIGHT_PATH && 
                                                (i>>1) > 0 &&
                                                ptrs[(i>>1)-1][j>>1] & RIGHT_PATH) { // right
                                            printf("%s", BLUE('|').data());
                                        } else { // none
                                            printf("|");
                                        }
                                        break;
                                    case ' ':
                                            printf("%c", ptr_str[i][j]);
                                        break;
                                }
                            }
                        }
                        printf("\n");
                    }
                }
                printf("score: %d\n", curr_score);

                /* // BACKTRACK */
                
                /* // init */
                /* std::vector<int> cig(ref_len+alt_len); */
                /* int cig_ptr = ref_len + alt_len - 1; */
                /* int score = s; */
                /* int diag = (ref_len-alt_len+score) / 2; // idx of last cell in WF */
                /* int ptr; */

                /* while (score > 0) { */

                /*     // find previous wavefront */
                /*     int off = offs[score][diag]; */
                /*     int prev_off, up_off, left_off; */
                /*     if (diag == 0) { // left edge, must go up */
                /*         if (score == 0) { */
                /*             ERROR("score should not be zero."); */
                /*         } */
                /*         up_off = offs[score-1][diag]+1; */
                /*         prev_off = up_off; */
                /*         ptr = PTR_UP; */

                /*     } else if (diag == score) { // right edge, must go left */
                /*         left_off = offs[score-1][diag-1]; */
                /*         prev_off = left_off; */
                /*         ptr = PTR_LEFT; */

                /*     } else { // get predecessor */
                /*         left_off = offs[score-1][diag-1]; */
                /*         up_off = offs[score-1][diag]+1; */
                /*         if (left_off > up_off) { */
                /*             prev_off = left_off; */
                /*             ptr = PTR_LEFT; */
                /*         } else { */
                /*             prev_off = up_off; */
                /*             ptr = PTR_UP; */
                /*         } */
                /*     } */

                /*     // slide up diagonally */
                /*     while (off > prev_off) { */
                /*         cig[cig_ptr--] = PTR_DIAG; */
                /*         cig[cig_ptr--] = PTR_DIAG; */
                /*         off--; */
                /*     } */

                /*     // go to previous wavefront */
                /*     cig[cig_ptr--] = ptr; */
                /*     score--; */
                /*     switch(ptr) { */
                /*     case PTR_LEFT: */
                /*         diag--; */
                /*         break; */
                /*     case PTR_UP: */
                /*         off--; */
                /*         break; */
                /*     default: */
                /*         ERROR("Pointer type unexpected: ptr=%i", ptr); */
                /*         break; */
                /*     } */

                /* } */
                /* // slide up diagonally to end */
                /* for (int i = 0; i <= offs[0][0]; i++) { */
                /*     cig[cig_ptr--] = PTR_DIAG; */
                /*     cig[cig_ptr--] = PTR_DIAG; */
                /* } */

                /* // get ref/alt strings for printing */
                /* int ref_ptr = 0; */
                /* int alt_ptr = 0; */
                /* int new_inss = 0; */
                /* int new_dels = 0; */
                /* std::string new_ref_out_str, new_alt_out_str; */
                /* for(size_t i = 0; i < cig.size(); i++) { */
                /*     switch(cig[i]) { */
                /*         case PTR_DIAG: */
                /*             new_ref_out_str += ref->fasta.at(ctg)[beg+ref_ptr++]; */
                /*             new_alt_out_str += alt_str[alt_ptr++]; */
                /*             i++; */
                /*             break; */
                /*         case PTR_UP: */
                /*             new_ref_out_str += " "; */
                /*             new_alt_out_str += GREEN(alt_str[alt_ptr++]); */
                /*             new_inss++; */
                /*             break; */
                /*         case PTR_LEFT: */
                /*             new_ref_out_str += RED(ref->fasta.at(ctg)[beg+ref_ptr++]); */
                /*             new_alt_out_str += " "; */
                /*             new_dels++; */
                /*             break; */
                /*     } */
                /* } */

                /* if (g.print_verbosity >= 2) { */
                /*     printf("\nPTRS: "); */
                /*     for(size_t i = 0; i < cig.size(); i++) { */
                /*         printf("%i ", cig[i]); */
                /*     } */

                /*     printf("\nCIGAR: "); */
                /*     for(size_t i = 0; i < cig.size(); i++) { */
                /*         switch(cig[i]) { */
                /*             case PTR_DIAG: // match is two movements */
                /*                 if (cig[++i] != PTR_DIAG) */
                /*                     ERROR("cig should be PTR_DIAG"); */
                /*                 printf("M"); break; */
                /*             case PTR_UP: */
                /*                 printf("I"); break; */
                /*             case PTR_LEFT: */
                /*                 printf("D"); break; */
                /*         } */
                /*     } */
                /*     printf("\n"); */
                /* } */

                /* // print cluster info, and old/new alignments */
                /* if (g.print_verbosity >= 1) { */
                /*     if (subs*2 + inss + dels != s) { */
                /*         const char* var_ref; */
                /*         const char* var_alt; */
                /*         printf("\n\n  Group %i: %d variants, %s:%d-%d\n\n" */
                /*                 "    old edit distance: %d (%dS %dI %dD)\n", */
                /*                 int(var_grp), end_idx-beg_idx, ctg.data(), */ 
                /*                 beg, end, subs*2 + inss + dels, subs, inss, dels); */
                /*         for (int var = beg_idx; var < end_idx; var++) { */
                /*             if (vars.refs[var].size() == 0) var_ref = "-"; */ 
                /*             else var_ref = vars.refs[var].data(); */
                /*             if (vars.alts[var].size() == 0) var_alt = "-"; */ 
                /*             else var_alt = vars.alts[var].data(); */
                /*             printf("      %s:%i hap%i %s\t%s\t%s\n", */ 
                /*                     ctg.data(), vars.poss[var], h, */ 
                /*                     type_strs[vars.types[var]].data(), */ 
                /*                     var_ref, var_alt); */
                /*         } */
                /*         printf("      REF: %s\n      ALT: %s\n", */ 
                /*                 ref_out_str.data(), alt_out_str.data()); */
                /*         printf("\n    new edit distance: %i (%iI %iD)\n", */ 
                /*                 s, new_inss, new_dels); */
                /*         printf("      REF: %s\n      ALT: %s\n", */ 
                /*                 new_ref_out_str.data(), new_alt_out_str.data()); */
                /*     } */
                /* } */

                /* // update counters */
                /* old_ed += subs*2 + inss + dels; */
                /* new_ed += s; */
                /* if (subs*2 + inss + dels > s) new_ed_clusters++; */
                /* clusters++; */

                /* // write new alignment to VCF */
                /* int ref_idx = 0, alt_idx = 0; */
                /* for(size_t cig_idx = 0; cig_idx < cig.size();) { */
                /*     int indel_len = 0; */
                /*     switch (cig[cig_idx]) { */

                /*         case PTR_DIAG: // no variant, update pointers */
                /*             cig_idx += 2; */
                /*             ref_idx++; */
                /*             alt_idx++; */
                /*             break; */

                /*         case PTR_LEFT: // deletion */
                /*             cig_idx++; indel_len++; */

                /*             // multi-base deletion */
                /*             while (cig_idx < cig.size() && cig[cig_idx] == PTR_LEFT) { */
                /*                 cig_idx++; indel_len++; */
                /*             } */
                /*             results.hapcalls[h][ctg].add_var(beg+ref_idx, */
                /*                     indel_len, h, TYPE_DEL, */ 
                /*                     ref_str.substr(ref_idx, indel_len), */
                /*                     "", 60, 60); */
                /*             ref_idx += indel_len; */
                /*             break; */

                /*         case PTR_UP: // insertion */
                /*             cig_idx++; indel_len++; */

                /*             // multi-base insertion */
                /*             while (cig_idx < cig.size() && cig[cig_idx] == PTR_UP) { */
                /*                 cig_idx++; indel_len++; */
                /*             } */
                /*             results.hapcalls[h][ctg].add_var(beg+ref_idx, */
                /*                     0, h, TYPE_INS, "", */ 
                /*                     alt_str.substr(alt_idx, indel_len), 60, 60); */
                /*             alt_idx += indel_len; */
                /*             break; */
                /*     } */
                /* } */


            } // cluster
        } // contig
    } // hap
    INFO("Edit dist reduced in %i of %i clusters, from %i to %i.", 
            new_ed_clusters, clusters, old_ed, new_ed);

    return results;

}

/******************************************************************************/

clusterData edit_dist(vcfData* calls, vcfData* truth, fastaData* ref) {

    // iterate over each contig
    int distance = 0;
    clusterData clusters(calls->contigs);
    for (std::string ctg : calls->contigs) {

        // initialize variant call info
        variantCalls* cal1_vars = &calls->hapcalls[0][ctg];
        variantCalls* cal2_vars = &calls->hapcalls[1][ctg];
        variantCalls* hap1_vars;
        variantCalls* hap2_vars;
        try {
            hap1_vars = &truth->hapcalls[0][ctg];
            hap2_vars = &truth->hapcalls[1][ctg];
        } catch (const std::exception & e) {
            ERROR("truth VCF does not contain contig '%s'", ctg.data());
        }

        // for each cluster of variants (merge calls and truth haps)
        size_t cal1_clust_beg_idx = 0;
        size_t cal2_clust_beg_idx = 0;
        size_t hap1_clust_beg_idx = 0;
        size_t hap2_clust_beg_idx = 0;
        while (cal1_clust_beg_idx < cal1_vars->clusters.size()-1 || // last cluster added for end
               cal2_clust_beg_idx < cal2_vars->clusters.size()-1 ||
               hap1_clust_beg_idx < hap1_vars->clusters.size()-1 ||
               hap2_clust_beg_idx < hap2_vars->clusters.size()-1) {

            // set all start positions
            size_t cal1_clust_end_idx = cal1_clust_beg_idx;
            size_t cal2_clust_end_idx = cal2_clust_beg_idx;
            size_t hap1_clust_end_idx = hap1_clust_beg_idx;
            size_t hap2_clust_end_idx = hap2_clust_beg_idx;

            int cal1_pos = (cal1_clust_beg_idx < cal1_vars->clusters.size()-1) ? 
                    cal1_vars->poss[cal1_vars->clusters[cal1_clust_end_idx]]-1 : 
                    std::numeric_limits<int>::max();
            int cal2_pos = (cal2_clust_beg_idx < cal2_vars->clusters.size()-1) ? 
                    cal2_vars->poss[cal2_vars->clusters[cal2_clust_end_idx]]-1 : 
                    std::numeric_limits<int>::max();
            int hap1_pos = (hap1_clust_beg_idx < hap1_vars->clusters.size()-1) ? 
                    hap1_vars->poss[hap1_vars->clusters[hap1_clust_end_idx]]-1 : 
                    std::numeric_limits<int>::max();
            int hap2_pos = (hap2_clust_beg_idx < hap2_vars->clusters.size()-1) ? 
                    hap2_vars->poss[hap2_vars->clusters[hap2_clust_end_idx]]-1 : 
                    std::numeric_limits<int>::max();

            // initialize cluster merging with first to start
            int curr_end_pos = 0;
            if (cal1_pos <= hap1_pos && cal1_pos <= cal2_pos && cal1_pos <= hap2_pos) {
                cal1_clust_end_idx += 1;
                curr_end_pos = cal1_vars->poss[cal1_vars->clusters[cal1_clust_end_idx]-1] +
                        cal1_vars->rlens[cal1_vars->clusters[cal1_clust_end_idx]-1] + 1;
                cal1_pos = (cal1_clust_end_idx < cal1_vars->clusters.size()-1) ? 
                    cal1_vars->poss[cal1_vars->clusters[cal1_clust_end_idx]]-1 : 
                    std::numeric_limits<int>::max();
            } 
            else if (cal2_pos <= hap1_pos && cal2_pos <= cal1_pos && 
                    cal2_pos <= hap2_pos) {
                cal2_clust_end_idx += 1;
                curr_end_pos = cal2_vars->poss[cal2_vars->clusters[cal2_clust_end_idx]-1] +
                        cal2_vars->rlens[cal2_vars->clusters[cal2_clust_end_idx]-1] + 1;
                cal2_pos = (cal2_clust_end_idx < cal2_vars->clusters.size()-1) ? 
                    cal2_vars->poss[cal2_vars->clusters[cal2_clust_end_idx]]-1 : 
                    std::numeric_limits<int>::max();
            } 
            else if (hap1_pos <= cal1_pos && hap1_pos <= cal2_pos && 
                    hap1_pos <= hap2_pos) {
                hap1_clust_end_idx += 1;
                curr_end_pos = hap1_vars->poss[hap1_vars->clusters[hap1_clust_end_idx]-1] +
                        hap1_vars->rlens[hap1_vars->clusters[hap1_clust_end_idx]-1] + 1;
                hap1_pos = (hap1_clust_end_idx < hap1_vars->clusters.size()-1) ? 
                    hap1_vars->poss[hap1_vars->clusters[hap1_clust_end_idx]]-1 : 
                    std::numeric_limits<int>::max();
            } 
            else {
                hap2_clust_end_idx += 1;
                curr_end_pos = hap2_vars->poss[hap2_vars->clusters[hap2_clust_end_idx]-1] +
                        hap2_vars->rlens[hap2_vars->clusters[hap2_clust_end_idx]-1] + 1;
                hap2_pos = (hap2_clust_end_idx < hap2_vars->clusters.size()-1) ? 
                    hap2_vars->poss[hap2_vars->clusters[hap2_clust_end_idx]]-1 : 
                    std::numeric_limits<int>::max();
            }


            // keep expanding cluster while possible
            bool just_merged = true;
            while (just_merged) {
                just_merged = false;
                while (hap1_pos < curr_end_pos + g.gap) {
                    hap1_clust_end_idx += 1;
                    curr_end_pos = std::max(curr_end_pos,
                            hap1_vars->poss[hap1_vars->clusters[hap1_clust_end_idx]-1] + 
                            hap1_vars->rlens[hap1_vars->clusters[hap1_clust_end_idx]-1] + 1);
                    hap1_pos = (hap1_clust_end_idx < hap1_vars->clusters.size()-1) ? 
                        hap1_vars->poss[hap1_vars->clusters[hap1_clust_end_idx]]-1 : 
                        std::numeric_limits<int>::max();
                    just_merged = true;
                }
                while (hap2_pos < curr_end_pos + g.gap) {
                    hap2_clust_end_idx += 1;
                    curr_end_pos = std::max(curr_end_pos,
                            hap2_vars->poss[hap2_vars->clusters[hap2_clust_end_idx]-1] + 
                            hap2_vars->rlens[hap2_vars->clusters[hap2_clust_end_idx]-1] + 1);
                    hap2_pos = (hap2_clust_end_idx < hap2_vars->clusters.size()-1) ? 
                        hap2_vars->poss[hap2_vars->clusters[hap2_clust_end_idx]]-1 : 
                        std::numeric_limits<int>::max();
                    just_merged = true;
                }
                while (cal1_pos < curr_end_pos + g.gap) {
                    cal1_clust_end_idx += 1;
                    curr_end_pos = std::max(curr_end_pos, 
                            cal1_vars->poss[cal1_vars->clusters[cal1_clust_end_idx]-1] + 
                            cal1_vars->rlens[cal1_vars->clusters[cal1_clust_end_idx]-1] + 1);
                    cal1_pos = (cal1_clust_end_idx < cal1_vars->clusters.size()-1) ? 
                        cal1_vars->poss[cal1_vars->clusters[cal1_clust_end_idx]]-1 : 
                        std::numeric_limits<int>::max();
                    just_merged = true;
                }
                while (cal2_pos < curr_end_pos + g.gap) {
                    cal2_clust_end_idx += 1;
                    curr_end_pos = std::max(curr_end_pos,
                            cal2_vars->poss[cal2_vars->clusters[cal2_clust_end_idx]-1] + 
                            cal2_vars->rlens[cal2_vars->clusters[cal2_clust_end_idx]-1] + 1);
                    cal2_pos = (cal2_clust_end_idx < cal2_vars->clusters.size()-1) ? 
                        cal2_vars->poss[cal2_vars->clusters[cal2_clust_end_idx]]-1 : 
                        std::numeric_limits<int>::max();
                    just_merged = true;
                }
            }

            // get supercluster start/end positions (allowing empty haps)
            int beg_pos = std::numeric_limits<int>::max();
            int end_pos = -1;
            if (cal1_clust_end_idx - cal1_clust_beg_idx) { // cal1 vars present
                beg_pos = std::min(beg_pos, 
                        cal1_vars->poss[cal1_vars->clusters[cal1_clust_beg_idx]]-1);
                end_pos = std::max(end_pos, 
                        cal1_vars->poss[cal1_vars->clusters[cal1_clust_end_idx]-1] + 
                        cal1_vars->rlens[cal1_vars->clusters[cal1_clust_end_idx]-1]+1);
            }
            if (cal2_clust_end_idx - cal2_clust_beg_idx) { // cal2 vars present
                beg_pos = std::min(beg_pos, 
                        cal2_vars->poss[cal2_vars->clusters[cal2_clust_beg_idx]]-1);
                end_pos = std::max(end_pos, 
                        cal2_vars->poss[cal2_vars->clusters[cal2_clust_end_idx]-1] + 
                        cal2_vars->rlens[cal2_vars->clusters[cal2_clust_end_idx]-1]+1);
            }
            if (hap1_clust_end_idx - hap1_clust_beg_idx) { // hap1 vars present
                beg_pos = std::min(beg_pos, 
                        hap1_vars->poss[hap1_vars->clusters[hap1_clust_beg_idx]]-1);
                end_pos = std::max(end_pos, 
                        hap1_vars->poss[hap1_vars->clusters[hap1_clust_end_idx]-1] + 
                        hap1_vars->rlens[hap1_vars->clusters[hap1_clust_end_idx]-1]+1);
            }
            if (hap2_clust_end_idx - hap2_clust_beg_idx) { // hap2 vars present
                beg_pos = std::min(beg_pos, 
                        hap2_vars->poss[hap2_vars->clusters[hap2_clust_beg_idx]]-1);
                end_pos = std::max(end_pos, 
                        hap2_vars->poss[hap2_vars->clusters[hap2_clust_end_idx]-1] + 
                        hap2_vars->rlens[hap2_vars->clusters[hap2_clust_end_idx]-1]+1);
            }

            // generate ref string
            std::string ref_str = ref->fasta.at(ctg).substr(beg_pos, end_pos-beg_pos);

            // generate cal1 and cal2 strings and pointers
            int cal1_var_idx = cal1_vars->clusters[cal1_clust_beg_idx];
            int cal2_var_idx = cal2_vars->clusters[cal2_clust_beg_idx];
            std::string cal1 = "", cal2 = "", cal1_str = "", cal2_str = ""; 
            std::vector<int> cal1_ptrs, cal2_ptrs;
            for (int cal1_ref_pos = beg_pos, cal2_ref_pos = beg_pos; cal1_ref_pos < end_pos || cal2_ref_pos < end_pos; ) {

                // CONSIDER HAP1 ONLY, PRIOR REFERENCE POSITION
                if (cal1_ref_pos < cal2_ref_pos) {
                    if (cal1_ref_pos == cal1_vars->poss[cal1_var_idx]) { // in cal1 variant
                        switch (cal1_vars->types[cal1_var_idx]) {
                            case TYPE_INS:
                                cal1 += cal1_vars->alts[cal1_var_idx];
                                cal1_ptrs.insert(cal1_ptrs.end(), cal1_vars->alts[cal1_var_idx].size(), -1);
                                cal1_str += GREEN(cal1_vars->alts[cal1_var_idx]);
                                break;
                            case TYPE_DEL:
                                cal1_str += std::string(cal1_vars->refs[cal1_var_idx].size(), ' ');
                                cal1_ref_pos += cal1_vars->refs[cal1_var_idx].size();
                                break;
                            case TYPE_SUB:
                                cal1 += cal1_vars->alts[cal1_var_idx];
                                cal1_ptrs.insert(cal1_ptrs.end(), cal1_vars->alts[cal1_var_idx].size(), -1);
                                cal1_str += GREEN(cal1_vars->alts[cal1_var_idx]);
                                cal1_ref_pos++;
                                break;
                            case TYPE_GRP:
                                cal1 += cal1_vars->alts[cal1_var_idx];
                                cal1_ptrs.insert(cal1_ptrs.end(), cal1_vars->alts[cal1_var_idx].size(), -1);
                                cal1_str += std::string(cal1_vars->refs[cal1_var_idx].size(), ' ') 
                                    + GREEN(cal1_vars->alts[cal1_var_idx]);
                                cal1_ref_pos += cal1_vars->refs[cal1_var_idx].size();
                                break;
                        }
                        cal1_var_idx++; // next variant
                    } else { // no cal1 variant, in cal2 variant
                        try {
                            cal1 += ref->fasta.at(ctg)[cal1_ref_pos];
                            cal1_ptrs.push_back(-1);
                            cal1_str += ref->fasta.at(ctg)[cal1_ref_pos];
                            cal1_ref_pos++;
                        } catch (const std::out_of_range & e) {
                            ERROR("contig %s not present in reference FASTA",
                                    ctg.data());
                            exit(1);
                        }
                    }
                }

                // CONSIDER HAP2 ONLY, PRIOR REFERENCE POSITION
                else if (cal2_ref_pos < cal1_ref_pos) {
                    if (cal2_ref_pos == cal2_vars->poss[cal2_var_idx]) { // in cal2 variant
                        switch (cal2_vars->types[cal2_var_idx]) {
                            case TYPE_INS:
                                cal2 += cal2_vars->alts[cal2_var_idx];
                                cal2_ptrs.insert(cal2_ptrs.end(), cal2_vars->alts[cal2_var_idx].size(), -1);
                                cal2_str += GREEN(cal2_vars->alts[cal2_var_idx]);
                                break;
                            case TYPE_DEL:
                                cal2_str += std::string(cal2_vars->refs[cal2_var_idx].size(), ' ');
                                cal2_ref_pos += cal2_vars->refs[cal2_var_idx].size();
                                break;
                            case TYPE_SUB:
                                cal2 += cal2_vars->alts[cal2_var_idx];
                                cal2_ptrs.insert(cal2_ptrs.end(), cal2_vars->alts[cal2_var_idx].size(), -1);
                                cal2_str += GREEN(cal2_vars->alts[cal2_var_idx]);
                                cal2_ref_pos++;
                                break;
                            case TYPE_GRP:
                                cal2 += cal2_vars->alts[cal2_var_idx];
                                cal2_ptrs.insert(cal2_ptrs.end(), cal2_vars->alts[cal2_var_idx].size(), -1);
                                cal2_str += std::string(cal2_vars->refs[cal2_var_idx].size(), ' ') 
                                    + GREEN(cal2_vars->alts[cal2_var_idx]);
                                cal2_ref_pos += cal2_vars->refs[cal2_var_idx].size();
                                break;
                        }
                        cal2_var_idx++; // next variant
                    } else { // match
                        try {
                            cal2 += ref->fasta.at(ctg)[cal2_ref_pos];
                            cal2_ptrs.push_back(-1);
                            cal2_str += ref->fasta.at(ctg)[cal2_ref_pos];
                            cal2_ref_pos++;
                        } catch (const std::out_of_range & e) {
                            ERROR("contig %s not present in reference FASTA",
                                    ctg.data());
                            exit(1);
                        }
                    }
                }

                // REFERENCE POSITIONS MATCH! POTENTIAL TRANSITIONS
                else {
                    bool cal1_var = false;
                    if (cal1_ref_pos == cal1_vars->poss[cal1_var_idx]) { // in cal1 variant
                        cal1_var = true;
                        switch (cal1_vars->types[cal1_var_idx]) {
                            case TYPE_INS:
                                cal1 += cal1_vars->alts[cal1_var_idx];
                                cal1_ptrs.insert(cal1_ptrs.end(), cal1_vars->alts[cal1_var_idx].size(), -1);
                                cal1_str += GREEN(cal1_vars->alts[cal1_var_idx]);
                                break;
                            case TYPE_DEL:
                                cal1_str += std::string(cal1_vars->refs[cal1_var_idx].size(), ' ');
                                cal1_ref_pos += cal1_vars->refs[cal1_var_idx].size();
                                break;
                            case TYPE_SUB:
                                cal1 += cal1_vars->alts[cal1_var_idx];
                                cal1_ptrs.insert(cal1_ptrs.end(), cal1_vars->alts[cal1_var_idx].size(), -1);
                                cal1_str += GREEN(cal1_vars->alts[cal1_var_idx]);
                                cal1_ref_pos++;
                                break;
                            case TYPE_GRP:
                                cal1 += cal1_vars->alts[cal1_var_idx];
                                cal1_ptrs.insert(cal1_ptrs.end(), cal1_vars->alts[cal1_var_idx].size(), -1);
                                cal1_str += std::string(cal1_vars->refs[cal1_var_idx].size(), ' ') 
                                    + GREEN(cal1_vars->alts[cal1_var_idx]);
                                cal1_ref_pos += cal1_vars->refs[cal1_var_idx].size();
                                break;
                        }
                        cal1_var_idx++; // next variant
                    } 

                    bool cal2_var = false;
                    if (cal2_ref_pos == cal2_vars->poss[cal2_var_idx]) { // in cal2 variant
                        cal2_var = true;
                        switch (cal2_vars->types[cal2_var_idx]) {
                            case TYPE_INS:
                                cal2 += cal2_vars->alts[cal2_var_idx];
                                cal2_ptrs.insert(cal2_ptrs.end(), cal2_vars->alts[cal2_var_idx].size(), -1);
                                cal2_str += GREEN(cal2_vars->alts[cal2_var_idx]);
                                break;
                            case TYPE_DEL:
                                cal2_str += std::string(cal2_vars->refs[cal2_var_idx].size(), ' ');
                                cal2_ref_pos += cal2_vars->refs[cal2_var_idx].size();
                                break;
                            case TYPE_SUB:
                                cal2 += cal2_vars->alts[cal2_var_idx];
                                cal2_ptrs.insert(cal2_ptrs.end(), cal2_vars->alts[cal2_var_idx].size(), -1);
                                cal2_str += GREEN(cal2_vars->alts[cal2_var_idx]);
                                cal2_ref_pos++;
                                break;
                            case TYPE_GRP:
                                cal2 += cal2_vars->alts[cal2_var_idx];
                                cal2_ptrs.insert(cal2_ptrs.end(), cal2_vars->alts[cal2_var_idx].size(), -1);
                                cal2_str += std::string(cal2_vars->refs[cal2_var_idx].size(), ' ') 
                                    + GREEN(cal2_vars->alts[cal2_var_idx]);
                                cal2_ref_pos += cal2_vars->refs[cal2_var_idx].size();
                                break;
                        }
                        cal2_var_idx++; // next variant

                    } 
                    
                    // ONE HAPLOTYPE WAS A VARIANT, INVALID POINTERS
                    if (!cal1_var && cal2_var) {
                        try {
                            cal1 += ref->fasta.at(ctg)[cal1_ref_pos];
                            cal1_ptrs.push_back(-1);
                            cal1_str += ref->fasta.at(ctg)[cal1_ref_pos];
                            cal1_ref_pos++;
                        } catch (const std::out_of_range & e) {
                            ERROR("contig %s not present in reference FASTA",
                                    ctg.data());
                            exit(1);
                        }
                    }
                    if (cal1_var && !cal2_var) {
                        try {
                            cal2 += ref->fasta.at(ctg)[cal2_ref_pos];
                            cal2_ptrs.push_back(-1);
                            cal2_str += ref->fasta.at(ctg)[cal2_ref_pos];
                            cal2_ref_pos++;
                        } catch (const std::out_of_range & e) {
                            ERROR("contig %s not present in reference FASTA",
                                    ctg.data());
                            exit(1);
                        }
                    }

                    // BOTH MATCH REFERENCE, ADD POINTERS
                    if (!cal1_var && !cal2_var) { // add pointers
                        try {
                            cal1_ptrs.push_back(cal2.size());
                            cal2_ptrs.push_back(cal1.size());
                            cal1 += ref->fasta.at(ctg)[cal1_ref_pos];
                            cal1_str += BLUE(ref->fasta.at(ctg)[cal1_ref_pos]);
                            cal1_ref_pos++;
                            cal2 += ref->fasta.at(ctg)[cal2_ref_pos];
                            cal2_str += BLUE(ref->fasta.at(ctg)[cal2_ref_pos]);
                            cal2_ref_pos++;
                        } catch (const std::out_of_range & e) {
                            ERROR("contig %s not present in reference FASTA",
                                    ctg.data());
                            exit(1);
                        }
                    }
                }
            }

            // generate hap1 and hap2 strings and pointers
            int hap1_var_idx = hap1_vars->clusters[hap1_clust_beg_idx];
            int hap2_var_idx = hap2_vars->clusters[hap2_clust_beg_idx];
            std::string hap1 = "", hap2 = "", hap1_str = "", hap2_str = ""; 
            std::vector<int> hap1_ptrs, hap2_ptrs;
            for (int hap1_ref_pos = beg_pos, hap2_ref_pos = beg_pos; hap1_ref_pos < end_pos || hap2_ref_pos < end_pos; ) {

                // CONSIDER HAP1 ONLY, PRIOR REFERENCE POSITION
                if (hap1_ref_pos < hap2_ref_pos) {
                    if (hap1_ref_pos == hap1_vars->poss[hap1_var_idx]) { // in hap1 variant
                        switch (hap1_vars->types[hap1_var_idx]) {
                            case TYPE_INS:
                                hap1 += hap1_vars->alts[hap1_var_idx];
                                hap1_ptrs.insert(hap1_ptrs.end(), hap1_vars->alts[hap1_var_idx].size(), -1);
                                hap1_str += GREEN(hap1_vars->alts[hap1_var_idx]);
                                break;
                            case TYPE_DEL:
                                hap1_str += std::string(hap1_vars->refs[hap1_var_idx].size(), ' ');
                                hap1_ref_pos += hap1_vars->refs[hap1_var_idx].size();
                                break;
                            case TYPE_SUB:
                                hap1 += hap1_vars->alts[hap1_var_idx];
                                hap1_ptrs.insert(hap1_ptrs.end(), hap1_vars->alts[hap1_var_idx].size(), -1);
                                hap1_str += GREEN(hap1_vars->alts[hap1_var_idx]);
                                hap1_ref_pos++;
                                break;
                            case TYPE_GRP:
                                hap1 += hap1_vars->alts[hap1_var_idx];
                                hap1_ptrs.insert(hap1_ptrs.end(), hap1_vars->alts[hap1_var_idx].size(), -1);
                                hap1_str += std::string(hap1_vars->refs[hap1_var_idx].size(), ' ') 
                                    + GREEN(hap1_vars->alts[hap1_var_idx]);
                                hap1_ref_pos += hap1_vars->refs[hap1_var_idx].size();
                                break;
                        }
                        hap1_var_idx++; // next variant
                    } else { // no hap1 variant, in hap2 variant
                        try {
                            hap1 += ref->fasta.at(ctg)[hap1_ref_pos];
                            hap1_ptrs.push_back(-1);
                            hap1_str += ref->fasta.at(ctg)[hap1_ref_pos];
                            hap1_ref_pos++;
                        } catch (const std::out_of_range & e) {
                            ERROR("contig %s not present in reference FASTA",
                                    ctg.data());
                            exit(1);
                        }
                    }
                }

                // CONSIDER HAP2 ONLY, PRIOR REFERENCE POSITION
                else if (hap2_ref_pos < hap1_ref_pos) {
                    if (hap2_ref_pos == hap2_vars->poss[hap2_var_idx]) { // in hap2 variant
                        switch (hap2_vars->types[hap2_var_idx]) {
                            case TYPE_INS:
                                hap2 += hap2_vars->alts[hap2_var_idx];
                                hap2_ptrs.insert(hap2_ptrs.end(), hap2_vars->alts[hap2_var_idx].size(), -1);
                                hap2_str += GREEN(hap2_vars->alts[hap2_var_idx]);
                                break;
                            case TYPE_DEL:
                                hap2_str += std::string(hap2_vars->refs[hap2_var_idx].size(), ' ');
                                hap2_ref_pos += hap2_vars->refs[hap2_var_idx].size();
                                break;
                            case TYPE_SUB:
                                hap2 += hap2_vars->alts[hap2_var_idx];
                                hap2_ptrs.insert(hap2_ptrs.end(), hap2_vars->alts[hap2_var_idx].size(), -1);
                                hap2_str += GREEN(hap2_vars->alts[hap2_var_idx]);
                                hap2_ref_pos++;
                                break;
                            case TYPE_GRP:
                                hap2 += hap2_vars->alts[hap2_var_idx];
                                hap2_ptrs.insert(hap2_ptrs.end(), hap2_vars->alts[hap2_var_idx].size(), -1);
                                hap2_str += std::string(hap2_vars->refs[hap2_var_idx].size(), ' ') 
                                    + GREEN(hap2_vars->alts[hap2_var_idx]);
                                hap2_ref_pos += hap2_vars->refs[hap2_var_idx].size();
                                break;
                        }
                        hap2_var_idx++; // next variant
                    } else { // match
                        try {
                            hap2 += ref->fasta.at(ctg)[hap2_ref_pos];
                            hap2_ptrs.push_back(-1);
                            hap2_str += ref->fasta.at(ctg)[hap2_ref_pos];
                            hap2_ref_pos++;
                        } catch (const std::out_of_range & e) {
                            ERROR("contig %s not present in reference FASTA",
                                    ctg.data());
                            exit(1);
                        }
                    }
                }

                // REFERENCE POSITIONS MATCH! POTENTIAL TRANSITIONS
                else {
                    bool hap1_var = false;
                    if (hap1_ref_pos == hap1_vars->poss[hap1_var_idx]) { // in hap1 variant
                        hap1_var = true;
                        switch (hap1_vars->types[hap1_var_idx]) {
                            case TYPE_INS:
                                hap1 += hap1_vars->alts[hap1_var_idx];
                                hap1_ptrs.insert(hap1_ptrs.end(), hap1_vars->alts[hap1_var_idx].size(), -1);
                                hap1_str += GREEN(hap1_vars->alts[hap1_var_idx]);
                                break;
                            case TYPE_DEL:
                                hap1_str += std::string(hap1_vars->refs[hap1_var_idx].size(), ' ');
                                hap1_ref_pos += hap1_vars->refs[hap1_var_idx].size();
                                break;
                            case TYPE_SUB:
                                hap1 += hap1_vars->alts[hap1_var_idx];
                                hap1_ptrs.insert(hap1_ptrs.end(), hap1_vars->alts[hap1_var_idx].size(), -1);
                                hap1_str += GREEN(hap1_vars->alts[hap1_var_idx]);
                                hap1_ref_pos++;
                                break;
                            case TYPE_GRP:
                                hap1 += hap1_vars->alts[hap1_var_idx];
                                hap1_ptrs.insert(hap1_ptrs.end(), hap1_vars->alts[hap1_var_idx].size(), -1);
                                hap1_str += std::string(hap1_vars->refs[hap1_var_idx].size(), ' ') 
                                    + GREEN(hap1_vars->alts[hap1_var_idx]);
                                hap1_ref_pos += hap1_vars->refs[hap1_var_idx].size();
                                break;
                        }
                        hap1_var_idx++; // next variant
                    } 

                    bool hap2_var = false;
                    if (hap2_ref_pos == hap2_vars->poss[hap2_var_idx]) { // in hap2 variant
                        hap2_var = true;
                        switch (hap2_vars->types[hap2_var_idx]) {
                            case TYPE_INS:
                                hap2 += hap2_vars->alts[hap2_var_idx];
                                hap2_ptrs.insert(hap2_ptrs.end(), hap2_vars->alts[hap2_var_idx].size(), -1);
                                hap2_str += GREEN(hap2_vars->alts[hap2_var_idx]);
                                break;
                            case TYPE_DEL:
                                hap2_str += std::string(hap2_vars->refs[hap2_var_idx].size(), ' ');
                                hap2_ref_pos += hap2_vars->refs[hap2_var_idx].size();
                                break;
                            case TYPE_SUB:
                                hap2 += hap2_vars->alts[hap2_var_idx];
                                hap2_ptrs.insert(hap2_ptrs.end(), hap2_vars->alts[hap2_var_idx].size(), -1);
                                hap2_str += GREEN(hap2_vars->alts[hap2_var_idx]);
                                hap2_ref_pos++;
                                break;
                            case TYPE_GRP:
                                hap2 += hap2_vars->alts[hap2_var_idx];
                                hap2_ptrs.insert(hap2_ptrs.end(), hap2_vars->alts[hap2_var_idx].size(), -1);
                                hap2_str += std::string(hap2_vars->refs[hap2_var_idx].size(), ' ') 
                                    + GREEN(hap2_vars->alts[hap2_var_idx]);
                                hap2_ref_pos += hap2_vars->refs[hap2_var_idx].size();
                                break;
                        }
                        hap2_var_idx++; // next variant

                    } 
                    
                    // ONE HAPLOTYPE WAS A VARIANT, INVALID POINTERS
                    if (!hap1_var && hap2_var) {
                        try {
                            hap1 += ref->fasta.at(ctg)[hap1_ref_pos];
                            hap1_ptrs.push_back(-1);
                            hap1_str += ref->fasta.at(ctg)[hap1_ref_pos];
                            hap1_ref_pos++;
                        } catch (const std::out_of_range & e) {
                            ERROR("contig %s not present in reference FASTA",
                                    ctg.data());
                            exit(1);
                        }
                    }
                    if (hap1_var && !hap2_var) {
                        try {
                            hap2 += ref->fasta.at(ctg)[hap2_ref_pos];
                            hap2_ptrs.push_back(-1);
                            hap2_str += ref->fasta.at(ctg)[hap2_ref_pos];
                            hap2_ref_pos++;
                        } catch (const std::out_of_range & e) {
                            ERROR("contig %s not present in reference FASTA",
                                    ctg.data());
                            exit(1);
                        }
                    }

                    // BOTH MATCH REFERENCE, ADD POINTERS
                    if (!hap1_var && !hap2_var) { // add pointers
                        try {
                            hap1_ptrs.push_back(hap2.size());
                            hap2_ptrs.push_back(hap1.size());
                            hap1 += ref->fasta.at(ctg)[hap1_ref_pos];
                            hap1_str += BLUE(ref->fasta.at(ctg)[hap1_ref_pos]);
                            hap1_ref_pos++;
                            hap2 += ref->fasta.at(ctg)[hap2_ref_pos];
                            hap2_str += BLUE(ref->fasta.at(ctg)[hap2_ref_pos]);
                            hap2_ref_pos++;
                        } catch (const std::out_of_range & e) {
                            ERROR("contig %s not present in reference FASTA",
                                    ctg.data());
                            exit(1);
                        }
                    }
                }
            }

            // ALIGNMENT
            std::vector<int> s(4);
            std::vector<std::string> cals {cal1, cal1, cal2, cal2};
            std::vector<std::string> haps {hap1, hap2, hap1, hap2};
            std::vector<int> call_lens = 
                    {int(cal1.size()), int(cal1.size()), int(cal2.size()), int(cal2.size())};
            std::vector<int> hap_lens = 
                    {int(hap1.size()), int(hap2.size()), int(hap1.size()), int(hap2.size())};
            std::vector< std::vector< std::vector<int> > > offs(4), ptrs(4);

            // for each combination of calls and haps
            for(int i = 0; i < 4; i++) {

                int mat_len = call_lens[i] + hap_lens[i] - 1;
                offs[i].push_back(std::vector<int>(mat_len,-2));
                offs[i][0][call_lens[i]-1] = -1;
                ptrs[i].push_back(std::vector<int>(mat_len,PTR_NONE));
                bool done = false;
                while (true) {

                    // EXTEND WAVEFRONT
                    for (int d = 0; d < mat_len; d++) {
                        int off = offs[i][s[i]][d];
                        int diag = d + 1 - call_lens[i];

                        // don't allow starting from untouched cells
                        if (off == -2) continue;

                        // check that it's within matrix
                        if (diag + off + 1 < 0) continue;
                        if (off > call_lens[i] - 1) continue;
                        if (diag + off > hap_lens[i] - 1) continue;

                        // extend
                        while (off < call_lens[i] - 1 && 
                               diag + off < hap_lens[i] - 1) {
                            if (cals[i][off+1] == haps[i][diag+off+1]) off++;
                            else break;
                        }
                        offs[i][s[i]][d] = off;

                        // finish if done
                        if (off == call_lens[i] - 1 && 
                            off + diag == hap_lens[i] - 1)
                        { done = true; break; }

                    }
                    if (done) break;


                    // NEXT WAVEFRONT
                    // add wavefront, fill edge cells
                    offs[i].push_back(std::vector<int>(mat_len, -2));
                    // bottom left cells
                    if (s[i]+1 == call_lens[i]-1)
                        offs[i][s[i]+1][0] = s[i]+1;
                    // top right cells
                    if (s[i]+1 == mat_len-1)
                        offs[i][s[i]+1][mat_len-1] = s[i]+1;

                    ptrs[i].push_back(std::vector<int>(mat_len));
                    ptrs[i][s[i]+1][0] = PTR_UP;
                    ptrs[i][s[i]+1][s[i]+1] = PTR_LEFT;

                    // central cells
                    for (int d = 1; d < mat_len-1; d++) {
                        int offleft = offs[i][s[i]][d-1];
                        int offtop = (offs[i][s[i]][d+1] == -2) ? 
                            -2 : offs[i][s[i]][d+1]+1;
                        if (offleft >= offtop) {
                            offs[i][s[i]+1][d] = offleft;
                            ptrs[i][s[i]+1][d] = PTR_LEFT;
                        } else {
                            offs[i][s[i]+1][d] = offtop;
                            ptrs[i][s[i]+1][d] = PTR_UP;
                        }
                    }
                    ++s[i];
                }
            }


            // PRINT RESULTS
            int dist = std::min(s[CAL1_HAP1]+s[CAL2_HAP2], 
                    s[CAL2_HAP1]+s[CAL1_HAP2]);
            if (g.print_verbosity >= 1 && dist) {
                // print cluster info
                printf("\n\nCAL1: %zu clusters\n", cal1_clust_end_idx-cal1_clust_beg_idx);
                for(size_t i = cal1_clust_beg_idx; i < cal1_clust_end_idx; i++) {
                    printf("\tGroup %zu: %d variants\n", i, cal1_vars->clusters[i+1]-cal1_vars->clusters[i]);
                    for(int j = cal1_vars->clusters[i]; j < cal1_vars->clusters[i+1]; j++) {
                        printf("\t\t%s %d\t%s\t%s\n", ctg.data(), cal1_vars->poss[j], 
                                cal1_vars->refs[j].size() ? cal1_vars->refs[j].data() : "_", 
                                cal1_vars->alts[j].size() ? cal1_vars->alts[j].data() : "_");
                    }
                }
                printf("CAL2: %zu clusters\n", cal2_clust_end_idx-cal2_clust_beg_idx);
                for(size_t i = cal2_clust_beg_idx; i < cal2_clust_end_idx; i++) {
                    printf("\tGroup %zu: %d variants\n", i, cal2_vars->clusters[i+1]-cal2_vars->clusters[i]);
                    for(int j = cal2_vars->clusters[i]; j < cal2_vars->clusters[i+1]; j++) {
                        printf("\t\t%s %d\t%s\t%s\n", ctg.data(), cal2_vars->poss[j], 
                                cal2_vars->refs[j].size() ? cal2_vars->refs[j].data() : "_", 
                                cal2_vars->alts[j].size() ? cal2_vars->alts[j].data() : "_");
                    }
                }
                printf("HAP1: %zu clusters\n", hap1_clust_end_idx-hap1_clust_beg_idx);
                for(size_t i = hap1_clust_beg_idx; i < hap1_clust_end_idx; i++) {
                    printf("\tGroup %zu: %d variants\n", i, hap1_vars->clusters[i+1]-hap1_vars->clusters[i]);
                    for(int j = hap1_vars->clusters[i]; j < hap1_vars->clusters[i+1]; j++) {
                        printf("\t\t%s %d\t%s\t%s\n", ctg.data(), hap1_vars->poss[j], 
                                hap1_vars->refs[j].size() ? hap1_vars->refs[j].data() : "_", 
                                hap1_vars->alts[j].size() ? hap1_vars->alts[j].data() : "_");
                    }
                }
                printf("HAP2: %zu clusters\n", hap2_clust_end_idx-hap2_clust_beg_idx);
                for(size_t i = hap2_clust_beg_idx; i < hap2_clust_end_idx; i++) {
                    printf("\tGroup %zu: %d variants\n", i, hap2_vars->clusters[i+1]-hap2_vars->clusters[i]);
                    for(int j = hap2_vars->clusters[i]; j < hap2_vars->clusters[i+1]; j++) {
                        printf("\t\t%s %d\t%s\t%s\n", ctg.data(), hap2_vars->poss[j], 
                                hap2_vars->refs[j].size() ? hap2_vars->refs[j].data() : "_", 
                                hap2_vars->alts[j].size() ? hap2_vars->alts[j].data() : "_");
                    }
                }
                
                printf("ORIG: %s\n", ref_str.data());
                printf("CAL1: %s\n", cal1_str.data());
                printf("CAL2: %s\n", cal2_str.data());
                printf("HAP1: %s\n", hap1_str.data());
                printf("HAP2: %s\n", hap2_str.data());
            }

            // DEBUG PRINT
            if (g.print_verbosity >= 2 && dist) {
                for(int h = 0; h < 4; h++) { // 4 alignments
                    printf("\n%s ALIGNMENT (distance %d)\n", 
                            aln_strs[h].data(), s[h]);

                    // create array
                    std::vector< std::vector<char> > ptr_str;
                    for (int i = 0; i < call_lens[h]; i++)
                        ptr_str.push_back(std::vector<char>(hap_lens[h], '.'));

                    // modify array with pointers
                    int mat_len = call_lens[h] + hap_lens[h] - 1;
                    for (int si = 0; si <= s[h]; si++) {
                        for(int di = 0; di < mat_len; di++) {
                            int diag = di + 1 - call_lens[h];
                            int off = offs[h][si][di];

                            // check that indices are within bounds
                            int cal_pos = off;
                            int hap_pos = diag + off;
                            if (cal_pos < 0 || hap_pos < 0) continue;
                            if (cal_pos > call_lens[h]-1 || 
                                    hap_pos > hap_lens[h]-1) continue;

                            // special case: main diag, no previous edits
                            if (si == 0 && diag == 0) {
                                while (cal_pos >= 0) {
                                    ptr_str[cal_pos--][hap_pos--] = '\\';
                                }
                            }
                            // left edge
                            else if (cal_pos == 0) {
                                ptr_str[cal_pos][hap_pos] = '-';
                            } 
                            // top edge
                            else if (hap_pos == 0) {
                                ptr_str[cal_pos][hap_pos] = '|';
                            } 
                            else {
                                // follow diagonal
                                int top_off = (di < mat_len-1) ? offs[h][si-1][di+1]+1 : -2;
                                int left_off = (di > 0) ? offs[h][si-1][di-1] : -2;
                                while (cal_pos > 0 && hap_pos > 0 && 
                                        cal_pos > top_off && cal_pos > left_off) {
                                    ptr_str[cal_pos--][hap_pos--] = '\\';
                                }
                                // check left/up
                                if (cal_pos == top_off) {
                                    ptr_str[cal_pos][hap_pos] = '|';
                                } else if (cal_pos == left_off) {
                                    ptr_str[cal_pos][hap_pos] = '-';
                                }
                            }
                        }
                    }

                    // print array
                    for (int i = -1; i < call_lens[h]; i++) {
                        for (int j = -1; j < hap_lens[h]; j++) {
                            if (i < 0 && j < 0) {
                                printf("  ");
                            }
                            else if (i < 0) {
                                printf("%c", haps[h][j]);
                            } else if (j < 0) {
                                printf("\n%c ", cals[h][i]);
                            } else {
                                printf("%c", ptr_str[i][j]);
                            }
                        }
                    }
                    printf("\n");

                } // 4 alignments
            } // debug print

            // get cluster phasing
            int orig_phase_dist = s[CAL1_HAP1] + s[CAL2_HAP2];
            int swap_phase_dist = s[CAL2_HAP1] + s[CAL1_HAP2];
            int phase = PHASE_NONE; // default either way
            if (orig_phase_dist < swap_phase_dist) phase = PHASE_ORIG;
            if (swap_phase_dist < orig_phase_dist) phase = PHASE_SWAP;

            // save alignment information
            clusters.clusters[ctg].add(
                    cal1_vars, cal1_clust_beg_idx, cal1_clust_end_idx,
                    cal2_vars, cal2_clust_beg_idx, cal2_clust_end_idx,
                    hap1_vars, hap1_clust_beg_idx, hap1_clust_end_idx,
                    hap2_vars, hap2_clust_beg_idx, hap2_clust_end_idx,
                    phase, orig_phase_dist, swap_phase_dist);

            // reset for next merged cluster
            cal1_clust_beg_idx = cal1_clust_end_idx;
            cal2_clust_beg_idx = cal2_clust_end_idx;
            hap1_clust_beg_idx = hap1_clust_end_idx;
            hap2_clust_beg_idx = hap2_clust_end_idx;
            distance += std::min(orig_phase_dist, swap_phase_dist);

        } // each cluster
    } // each contig
    INFO("Total edit distance: %d", distance);

    return clusters;
}
