#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>

#include "htslib/vcf.h"

#include "variant.h"
#include "print.h"
#include "dist.h"

/******************************************************************************/

ctgVariants::ctgVariants() { 
    this->n = 0; 
}

void ctgVariants::add_cluster(int g) { this->clusters.push_back(g); }

void ctgVariants::add_var(int pos, int rlen, uint8_t hap, uint8_t type, uint8_t loc,
        std::string ref, std::string alt, uint8_t orig_gt, float gq, float vq) {
    this->poss.push_back(pos);
    this->rlens.push_back(rlen);
    this->haps.push_back(hap);
    this->types.push_back(type);
    this->locs.push_back(loc);
    this->refs.push_back(ref);
    this->alts.push_back(alt);
    this->orig_gts.push_back(orig_gt);
    this->gt_quals.push_back(gq);
    this->var_quals.push_back(std::min(vq, float(g.max_qual)));
    this->n++;

    this->errtypes.push_back(ERRTYPE_UN);
    this->credit.push_back(0);
    this->callq.push_back(0);
}

/******************************************************************************/

void variantData::left_shift() {

    // shift variants as far left as possible after realignment
    for (int hap = 0; hap < HAPS; hap++) {
        for (std::string ctg : this->contigs) {
            auto vars = this->ctg_variants[hap][ctg];
            for (int i = 0; i < vars->n; i++) {

                // shift INS
                if (vars->types[i] == TYPE_INS) {
                    bool match = true;
                    while (match && vars->poss[i] > 0 && 
                            (i == 0 || vars->poss[i] > 
                            vars->poss[i-1] + vars->rlens[i-1]+1)) {
                        int ins_size = vars->alts[i].size();
                        char ref_base = this->ref->fasta.at(ctg)[vars->poss[i]-1];
                        if (ref_base == vars->alts[i][ins_size-1]) {
                            vars->alts[i] = ref_base + 
                                    vars->alts[i].substr(0, ins_size-1);
                            vars->poss[i]--;
                        } else {
                            match = false;
                        }
                    }
                }

                // shift DEL
                if (vars->types[i] == TYPE_DEL) {
                    bool match = true;
                    while (match && vars->poss[i] > 0 && 
                            (i == 0 || vars->poss[i] > 
                            vars->poss[i-1] + vars->rlens[i-1]+1)) {
                        int del_size = vars->refs[i].size();
                        char ref_base = this->ref->fasta.at(ctg)[vars->poss[i]-1];
                        if (ref_base == vars->refs[i][del_size-1]) {
                            vars->refs[i] = ref_base + 
                                    vars->refs[i].substr(0, del_size-1);
                            vars->poss[i]--;
                        } else {
                            match = false;
                        }
                    }
                }
            }
        }
    }

    // for each variant
    for (int hap = 0; hap < HAPS; hap++) {
        for (std::string ctg : this->contigs) {
            auto vars = this->ctg_variants[hap][ctg];
            for (int i = 0; i < vars->n; i++) {

                // if we consume a ref base, and there's another variant at the
                // same position (which doesn't, by definition), move this var
                // afterwards
                if (vars->refs[i].size() && i+1 < vars->n && 
                            vars->poss[i+1] == vars->poss[i]) {
                    std::swap(vars->rlens[i], vars->rlens[i+1]);
                    std::swap(vars->haps[i],  vars->haps[i+1]);
                    std::swap(vars->types[i], vars->types[i+1]);
                    std::swap(vars->locs[i],  vars->locs[i+1]);
                    std::swap(vars->refs[i],  vars->refs[i+1]);
                    std::swap(vars->alts[i],  vars->alts[i+1]);
                    std::swap(vars->orig_gts[i],   vars->orig_gts[i+1]);
                    std::swap(vars->gt_quals[i],   vars->gt_quals[i+1]);
                    std::swap(vars->var_quals[i],  vars->var_quals[i+1]);
                }
            }
        }
    }
}

/******************************************************************************/

void variantData::write_vcf(std::string out_vcf_fn) {

    // VCF header
    FILE* out_vcf = fopen(out_vcf_fn.data(), "w");
    const std::chrono::time_point now{std::chrono::system_clock::now()};
    time_t tt = std::chrono::system_clock::to_time_t(now);
    tm local_time = *localtime(&tt);
    fprintf(out_vcf, "##fileformat=VCFv4.2\n");
    fprintf(out_vcf, "##fileDate=%04d%02d%02d\n", local_time.tm_year + 1900, 
            local_time.tm_mon + 1, local_time.tm_mday);
    for (size_t i = 0; i < this->contigs.size(); i++)
        fprintf(out_vcf, "##contig=<ID=%s,length=%d,ploidy=%d>\n", 
                this->contigs[i].data(), this->lengths[i], this->ploidy[i]);
    fprintf(out_vcf, "##FILTER=<ID=PASS,Description=\"All filters passed\">\n");
    fprintf(out_vcf, "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">\n");
    fprintf(out_vcf, "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\t%s\n",
            this->sample.data());

    // write variants
    for (std::string ctg : this->contigs) {
        std::vector<size_t> ptrs = {0, 0};
        int p = this->ploidy[std::find(contigs.begin(), contigs.end(), ctg) - contigs.begin()];
        while (ptrs[HAP1] < this->ctg_variants[HAP1][ctg]->poss.size() ||
                ptrs[HAP2] < this->ctg_variants[HAP2][ctg]->poss.size()) {

            // get next positions, set flags for which haps
            int pos_hap1 = ptrs[HAP1] < this->ctg_variants[HAP1][ctg]->poss.size() ? 
                this->ctg_variants[HAP1][ctg]->poss[ptrs[HAP1]] : std::numeric_limits<int>::max();
            int pos_hap2 = ptrs[HAP2] < this->ctg_variants[HAP2][ctg]->poss.size() ? 
                this->ctg_variants[HAP2][ctg]->poss[ptrs[HAP2]] : std::numeric_limits<int>::max();

            // indels include previous base, adjust position
            if (ptrs[HAP1] < this->ctg_variants[HAP1][ctg]->types.size() && 
                    (this->ctg_variants[HAP1][ctg]->types[ptrs[HAP1]] == TYPE_INS || 
                    this->ctg_variants[HAP1][ctg]->types[ptrs[HAP1]] == TYPE_DEL)) pos_hap1--;
            if (ptrs[HAP2] < this->ctg_variants[HAP2][ctg]->types.size() && 
                    (this->ctg_variants[HAP2][ctg]->types[ptrs[HAP2]] == TYPE_INS || 
                    this->ctg_variants[HAP2][ctg]->types[ptrs[HAP2]] == TYPE_DEL)) pos_hap2--;
            int pos = std::min(pos_hap1, pos_hap2);
            bool hap1 = (pos_hap1 == pos);
            bool hap2 = (pos_hap2 == pos);

            // add variants to output VCF file
            if (hap1 && hap2) {
                if (this->ctg_variants[HAP1][ctg]->refs[ptrs[HAP1]] == 
                        this->ctg_variants[HAP2][ctg]->refs[ptrs[HAP2]] &&
                        this->ctg_variants[HAP1][ctg]->alts[ptrs[HAP1]] == 
                        this->ctg_variants[HAP2][ctg]->alts[ptrs[HAP2]]) {
                    
                    // homozygous variant (1|1)
                    print_variant(out_vcf, ctg, pos, 
                            this->ctg_variants[HAP1][ctg]->types[ptrs[HAP1]],
                            this->ctg_variants[HAP1][ctg]->refs[ptrs[HAP1]],
                            this->ctg_variants[HAP1][ctg]->alts[ptrs[HAP1]],
                            this->ctg_variants[HAP1][ctg]->var_quals[ptrs[HAP1]], "1|1");
                    
                } else {
                    // two separate phased variants (0|1 + 1|0)
                    print_variant(out_vcf, ctg, pos, 
                            this->ctg_variants[HAP1][ctg]->types[ptrs[HAP1]],
                            this->ctg_variants[HAP1][ctg]->refs[ptrs[HAP1]],
                            this->ctg_variants[HAP1][ctg]->alts[ptrs[HAP1]],
                            this->ctg_variants[HAP1][ctg]->var_quals[ptrs[HAP1]], "1|0");
                    print_variant(out_vcf, ctg, pos, 
                            this->ctg_variants[HAP2][ctg]->types[ptrs[HAP2]],
                            this->ctg_variants[HAP2][ctg]->refs[ptrs[HAP2]],
                            this->ctg_variants[HAP2][ctg]->alts[ptrs[HAP2]],
                            this->ctg_variants[HAP2][ctg]->var_quals[ptrs[HAP2]], "0|1");
                }

            } else if (hap1) { // 1|0
                print_variant(out_vcf, ctg, pos, 
                        this->ctg_variants[HAP1][ctg]->types[ptrs[HAP1]],
                        this->ctg_variants[HAP1][ctg]->refs[ptrs[HAP1]],
                        this->ctg_variants[HAP1][ctg]->alts[ptrs[HAP1]],
                        this->ctg_variants[HAP1][ctg]->var_quals[ptrs[HAP1]], p == 1 ? "1" : "1|0");

            } else if (hap2) { // 0|1
                print_variant(out_vcf, ctg, pos, 
                        this->ctg_variants[HAP2][ctg]->types[ptrs[HAP2]],
                        this->ctg_variants[HAP2][ctg]->refs[ptrs[HAP2]],
                        this->ctg_variants[HAP2][ctg]->alts[ptrs[HAP2]],
                        this->ctg_variants[HAP2][ctg]->var_quals[ptrs[HAP2]],  p == 1 ? "1" :"0|1");
            }

            // update pointers
            if (hap1) ptrs[HAP1]++;
            if (hap2) ptrs[HAP2]++;
        }
    }
    fclose(out_vcf);
}


/*******************************************************************************/


void ctgVariants::print_var_info(FILE* out_fp, std::shared_ptr<fastaData> ref, 
        std::string ctg, int idx) {
    char ref_base;
    switch (this->types[idx]) {
    case TYPE_SUB:
        fprintf(out_fp, "%s\t%d\t.\t%s\t%s\t.\tPASS\t.\tGT:BD:BK:BC:QQ:SC:SP", 
                ctg.data(), this->poss[idx]+1, this->refs[idx].data(), 
                this->alts[idx].data());
        break;
    case TYPE_INS:
    case TYPE_DEL:
        ref_base = ref->fasta.at(ctg)[this->poss[idx]-1];
        fprintf(out_fp, "%s\t%d\t.\t%s\t%s\t.\tPASS\t.\tGT:BD:BK:BC:QQ:SC:SP", ctg.data(), 
                this->poss[idx], (ref_base + this->refs[idx]).data(), 
                (ref_base + this->alts[idx]).data());
        break;
    default:
        ERROR("print_variant not implemented for type %d", this->types[idx]);
    }
}


void ctgVariants::print_var_empty(FILE* out_fp, bool query /* = false */) {
    fprintf(out_fp, "\t.:.:.:.:.:.:.%s", query ? "\n" : "");
}


void ctgVariants::print_var_sample(FILE* out_fp, int idx, std::string gt, 
        int sc_idx, bool swap, bool query /* = false */) {

    std::string errtype;
    switch (this->errtypes[idx]) {
        case ERRTYPE_TP: errtype = "TP"; break; //if TP, leave value
        case ERRTYPE_FP: errtype = "FP"; break; //if FP, leave value
        case ERRTYPE_FN: errtype = "FN"; break; //if FN, leave value
        case ERRTYPE_PP: // for hap.py compatibility
             errtype = this->credit[idx] >= 0.5 ? "TP" : 
                 (query ? "FP" : "FN"); break;
    }//min is 0.000000, max is 1.000000, so changed it to >=1.000000 from 0.5.

    fprintf(out_fp, "\t%s:%s:%f:gm:%d:%d:%s%s", gt.data(), errtype.data(), 
            this->credit[idx], int(this->var_quals[idx]), sc_idx, query ? 
            (swap ? "1" : "0") : "." , query ? "\n" : "");
}


/*******************************************************************************/


void variantData::print_variant(FILE* out_fp, std::string ctg, int pos, int type,
        std::string ref, std::string alt, float qual, std::string gt) {

    char ref_base;
    switch (type) {
    case TYPE_SUB:
        fprintf(out_fp, "%s\t%d\t.\t%s\t%s\t%f\tPASS\t.\tGT\t%s\n", ctg.data(),
            pos+1, ref.data(), alt.data(), qual, gt.data());
        break;
    case TYPE_INS:
    case TYPE_DEL:
        try {
            ref_base = this->ref->fasta.at(ctg)[pos];
            fprintf(out_fp, "%s\t%d\t.\t%s\t%s\t%f\tPASS\t.\tGT\t%s\n", ctg.data(), 
                    pos+1, (ref_base + ref).data(), (ref_base + alt).data(), 
                    qual, gt.data());
        } catch (const std::out_of_range & e) {
            ERROR("Contig '%s' not in reference FASTA (print_variant)", ctg.data());
        }
        break;
    default:
        ERROR("print_variant not implemented for type %d", type);
    }
}

void variantData::set_header(const std::shared_ptr<variantData> vcf) {
    this->filename = vcf->filename;
    this->sample = vcf->sample;
    this->contigs = vcf->contigs;
    this->lengths = vcf->lengths;
    this->ploidy = vcf->ploidy;
    this->ref = vcf->ref;
    for (std::string ctg : this->contigs)
        for (int hap = 0; hap < 2; hap++)
            this->ctg_variants[hap][ctg] = 
                std::shared_ptr<ctgVariants>(new ctgVariants());
}


/* Copy variants from `cigar` string to `variantData`. */
void variantData::add_variants(
        const std::vector<int> & cigar, 
        int hap, int ref_pos,
        const std::string & ctg, 
        const std::string & query, 
        const std::string & ref, 
        int qual) {

    int query_idx = 0;
    int ref_idx = 0;
    for (size_t cig_idx = 0; cig_idx < cigar.size(); ) {
        int indel_len = 0;
        switch (cigar[cig_idx]) {

            case PTR_MAT: // no variant, update pointers
                cig_idx += 2;
                ref_idx++;
                query_idx++;
                break;

            case PTR_SUB: // substitution
                cig_idx += 2;
                this->ctg_variants[hap][ctg]->add_var(ref_pos+ref_idx, 1, hap, 
                        TYPE_SUB, BED_INSIDE, std::string(1,ref[ref_idx]), 
                        std::string(1,query[query_idx]), 
                        GT_REF_REF, g.max_qual, qual);
                ref_idx++;
                query_idx++;
                break;

            case PTR_DEL: // deletion
                cig_idx++; indel_len++;

                // multi-base deletion
                while (cig_idx < cigar.size() && cigar[cig_idx] == PTR_DEL) {
                    cig_idx++; indel_len++;
                }
                this->ctg_variants[hap][ctg]->add_var(ref_pos+ref_idx,
                        indel_len, hap, TYPE_DEL, BED_INSIDE,
                        ref.substr(ref_idx, indel_len),
                        "", GT_REF_REF, g.max_qual, qual);
                ref_idx += indel_len;
                break;

            case PTR_INS: // insertion
                cig_idx++; indel_len++;

                // multi-base insertion
                while (cig_idx < cigar.size() && cigar[cig_idx] == PTR_INS) {
                    cig_idx++; indel_len++;
                }
                this->ctg_variants[hap][ctg]->add_var(ref_pos+ref_idx,
                        0, hap, TYPE_INS, BED_INSIDE, "", 
                        query.substr(query_idx, indel_len), 
                        GT_REF_REF, g.max_qual, qual);
                query_idx += indel_len;
                break;
        }
    }
}

/******************************************************************************/

variantData::variantData() : ctg_variants(2) { ; }

variantData::variantData(std::string vcf_fn, 
        std::shared_ptr<fastaData> reference, 
        int callset) : ctg_variants(2) {

    // set reference fasta pointer
    this->ref = reference;
    this->filename = vcf_fn;

    if (callset < 0 || callset >= CALLSETS)
        ERROR("Invalid callset (%d).", callset);
    this->callset = callset;

    if (g.verbosity >= 1) INFO(" ");
    if (g.verbosity >= 1) INFO("%s[%s 0/8] Parsing %s VCF%s '%s'", COLOR_PURPLE,
            callset == QUERY ? "Q" : "T", callset_strs[callset].data(), 
            COLOR_WHITE, vcf_fn.data());
    htsFile* vcf = bcf_open(vcf_fn.data(), "r");

    // counters
    int nctg   = 0;                     // number of ctgs
    std::vector< std::vector<int> > 
        ntypes(2, std::vector<int>(type_strs.size(), 0));
    int n      = 0;                     // total number of records in file
    std::vector<int> npass  = {0, 0};   // records PASSing all filters

    // data
    std::vector<int> prev_end = {-g.cluster_min_gap*2, -g.cluster_min_gap*2};
    std::unordered_map<int, bool> prev_rids;
    int prev_rid = -1;
    std::unordered_map<int, int> ctglens;
    std::string ctg;
    std::vector<int> nregions(region_strs.size(), 0);
    std::vector<int> pass_min_qual = {0, 0};

    // quality data for each call
    int ngq_arr = 0;
    int ngq     = 0;
    int * gq    = (int*) malloc(sizeof(int));
    float * fgq = (float*) malloc(sizeof(float));
    bool int_qual = true;

    // genotype data for each call
    int ngt_arr   = 0;
    int ngt       = 0;
    std::vector<int> ngts(gt_strs.size(), 0);
    int * gt      = NULL;
    bool gt_warn  = false;

    /* int gq_missing_total = 0; */
    int overlapping_var_total = 0;
    int unknown_allele_total = 0;
    /* int unphased_gt_total = 0; */
    int small_var_total = 0;
    int large_var_total = 0;
    int wrong_ploidy_total = 0;
    
    // read header
    bcf1_t * rec  = NULL;
    bcf_hdr_t *hdr = bcf_hdr_read(vcf);
    int pass_filter_id = 0;
    bool pass = false;
    bool pass_found = false;
    for (int i = 0; i < hdr->nhrec; i++) {

        /* // DEBUG HEADER PRINT */
        /* printf("%s=%s\n", hdr->hrec[i]->key, hdr->hrec[i]->value); */
        /* for (int j = 0; j < hdr->hrec[i]->nkeys; j++) { */
        /*     printf("  %s=%s\n", hdr->hrec[i]->keys[j], hdr->hrec[i]->vals[j]); */
        /* } */

        // search all FILTER lines
        if (hdr->hrec[i]->type == BCF_HL_FLT) {

            // select PASS filter
            bool is_pass_filter = false;
            for (int j = 0; j < hdr->hrec[i]->nkeys; j++) {
                if (std::string(hdr->hrec[i]->keys[j]) == std::string("ID") &&  
                        std::string(hdr->hrec[i]->vals[j]) == std::string("PASS")) {
                    is_pass_filter = true;
                }
            }

            // save PASS filter index to keep only passing reads
            if (is_pass_filter) {
                for (int j = 0; j < hdr->hrec[i]->nkeys; j++) {
                    if (std::string(hdr->hrec[i]->keys[j]) == std::string("IDX")) {
                        pass_filter_id = std::stoi(hdr->hrec[i]->vals[j]);
                        pass_found = true;
                    }
                }
            }
        }

        // store contig lengths (for output VCF)
        else if (hdr->hrec[i]->type == BCF_HL_CTG) {
            int length = -1;
            int idx = -1;
            for (int j = 0; j < hdr->hrec[i]->nkeys; j++) {
                if (hdr->hrec[i]->keys[j] == std::string("IDX"))
                    idx = std::stoi(hdr->hrec[i]->vals[j]);
                else if (hdr->hrec[i]->keys[j] == std::string("length"))
                    length = std::stoi(hdr->hrec[i]->vals[j]);
            }
            if (length >= 0 && idx >= 0) {
                ctglens[idx] = length;
            } else {
                ERROR("%s VCF header contig line didn't have 'IDX' and 'length'",
                        callset_strs[callset].data());
            }
        }
    }
    if (!pass_found)
        ERROR("Failed to find PASS FILTER index in %s VCF '%s'",
                callset_strs[callset].data(), vcf_fn.data());
    if (bcf_hdr_nsamples(hdr) != 1) 
        ERROR("Expected 1 sample but found %d in %s VCF '%s'", bcf_hdr_nsamples(hdr),
                callset_strs[callset].data(), vcf_fn.data());
    this->sample = hdr->samples[0];

    // report names of all the ctgs in the VCF file
    const char **ctgnames = NULL;
    ctgnames = bcf_hdr_seqnames(hdr, &nctg);
    if (ctgnames == NULL) {
        ERROR("Failed to read %s VCF '%s' header", 
                callset_strs[callset].data(), vcf_fn.data());
        goto error1;
    }
    for(int i = 0; i < nctg; i++) {
        this->ctg_variants[HAP1][ctgnames[i]] = 
                std::shared_ptr<ctgVariants>(new ctgVariants());
        this->ctg_variants[HAP2][ctgnames[i]] = 
                std::shared_ptr<ctgVariants>(new ctgVariants());
    }

    // struct for storing each record
    rec = bcf_init();
    if (rec == NULL) {
        ERROR("Failed to read %s VCF '%s' records", 
                callset_strs[callset].data(), vcf_fn.data());
        goto error2;
    }
    
    while (bcf_read(vcf, hdr, rec) == 0) {

        ctg = ctgnames[rec->rid];
        if (rec->rid != prev_rid) {

            // start new contig
            prev_rid = rec->rid;
            if (prev_rids.find(rec->rid) != prev_rids.end()) {
                ERROR("Unsorted %s VCF '%s', contig '%s' already parsed", 
                        callset_strs[callset].data(), vcf_fn.data(), ctg.data());
            } else {
                this->contigs.push_back(ctg);
                this->ploidy.push_back(0);
                this->lengths.push_back(ctglens[rec->rid]);
                prev_end = {-g.cluster_min_gap*2, -g.cluster_min_gap*2};
            }
        }

        // unpack info (populates rec->d allele info)
        bcf_unpack(rec, BCF_UN_ALL);
        n++;

        // check that variant passed all filters
        pass = false;
        for (int i = 0; i < rec->d.n_flt; i++) {
            if (rec->d.flt[i] == pass_filter_id) pass = true;
        }
        if (rec->d.n_flt == 0) pass = true; // allow if no other filters
        if (!pass) continue;

        // check that variant exceeds min_qual
        float vq = rec->qual;
        if (std::isnan(vq)) vq = 0; // no quality reported (.)
        pass = vq >= g.min_qual;
        pass_min_qual[pass]++;
        if (!pass) continue;

        // parse GQ in either INT or FLOAT format
        if (int_qual) {
            ngq = bcf_get_format_int32(hdr, rec, "GQ", &gq, &ngq_arr);
            if (ngq == -2) {
                ngq = bcf_get_format_float(hdr, rec, "GQ", &fgq, &ngq_arr);
                gq[0] = int(fgq[0]);
                int_qual = false;
            }
        }
        else {
            ngq = bcf_get_format_float(hdr, rec, "GQ", &fgq, &ngq_arr);
            gq[0] = int(fgq[0]);
        }
        if ( ngq == -3 ) {
            /* if (g.verbosity > 1 || !gq_missing_total) */
            /*     WARN("No GQ tag in %s VCF at %s:%lld", */
            /*             callset_strs[callset].data(), ctg.data(), (long long)rec->pos); */
            /* gq_missing_total++; // only warn once */
            gq[0] = 0;
        }

        // parse GT
        ngt = bcf_get_format_int32(hdr, rec, "GT", &gt, &ngt_arr);
        if (ngt == -1) { // GT not defined in header
            if (!gt_warn) {
                gt_warn = true;
                WARN("'GT' tag not defined in header, assuming monoploid");
            }
        } else if (ngt < 0) { // other error
            ERROR("Failed to read %s GT at %s:%lld", 
                    callset_strs[callset].data(), ctg.data(), (long long)rec->pos);
        }

        // update ploidy info
        int ctg_idx = std::find(contigs.begin(), contigs.end(), ctg) - contigs.begin();
        if (ploidy[ctg_idx] != 0) { // already set, enforce it doesn't change
            if (std::abs(ngt) != ploidy[ctg_idx] && ctg[ctg.size()-1] != 'X') {
                if (g.verbosity > 1)
                    WARN("Expected ploidy %d for all variants on contig '%s',"
                          " found ploidy %d at %s:%lld in %s VCF.", ploidy[ctg_idx],
                        ctg.data(), std::abs(ngt), ctg.data(), (long long)rec->pos,
                        callset_strs[callset].data());
                wrong_ploidy_total += 1;
            }
        } else { // set ploidy for this contig
            ploidy[ctg_idx] = std::abs(ngt);
        }

        // parse genotype info
        int orig_gt = GT_REF_REF;
        bool same = false;
        if (ngt == -1) { // no info, assume monoploid
            orig_gt = GT_ALT1;
        } else if (ngt == 1) { // monoploid/haploid

            // set 1 if allele_idx > 0
            orig_gt = bcf_gt_allele(gt[0]) ? GT_ALT1 : GT_REF;

        } else if (ngt == 2) { // diploid

            // missing, ignore
            if (bcf_gt_is_missing(gt[0]) || bcf_gt_is_missing(gt[1])) {
                orig_gt = GT_MISSING;

            } else { // useful

                // allow setting N/N to 1/1 later
                if (bcf_gt_allele(gt[0]) == bcf_gt_allele(gt[1])) same = true;

                if (bcf_gt_allele(gt[0]) == 0) { // REF
                    switch (bcf_gt_allele(gt[1])) {
                        case 0: orig_gt = GT_REF_REF; break;
                        case 1: orig_gt = GT_REF_ALT1; break;
                        default: orig_gt = GT_OTHER; break;
                    }
                } else if (bcf_gt_allele(gt[0]) == 1) { // ALT1
                    switch (bcf_gt_allele(gt[1])) {
                        case 0: orig_gt = GT_ALT1_REF; break;
                        case 1: orig_gt = GT_ALT1_ALT1; break;
                        case 2: orig_gt = GT_ALT1_ALT2; break;
                        default: orig_gt = GT_OTHER; break;
                    }
                } else if (bcf_gt_allele(gt[0]) == 2) { // ALT2
                    orig_gt = (bcf_gt_allele(gt[1]) == 1) ? GT_ALT2_ALT1 : GT_OTHER;
                } else {
                    orig_gt = GT_OTHER;
                }
            }

        } else if (ngt > 2) { // polyploid
            ERROR("Expected monoploid/diploid %s VCF, found variant with ploidy %d",
                    callset_strs[callset].data(), ngt);
        }
        ngts[orig_gt]++;

        // parse variant type
        /* bool counted_unphased = false; */
        for (int hap = 0; hap < std::abs(ngt); hap++) { // allow single-allele chrX, chrY

            // set simplified GT (0|1, 1|0, or 1|1), (0|0 and .|. skipped later)
            int simple_gt = hap ? GT_REF_ALT1 : GT_ALT1_REF; // 0|1 or 1|0 default
            if (same) simple_gt = GT_ALT1_ALT1; // overwrite 1|1 if both agree

            // get ref and allele, skipping ref query
            std::string ref = rec->d.allele[0];
            int alt_idx = ngt < 0 ? 1 : bcf_gt_allele(gt[hap]); // if no GT, assume 1
            if (alt_idx < 0) {
                if (g.verbosity > 1)
                    WARN("Unknown allele (.) in %s VCF at %s:%lld, skipping",
                        callset_strs[callset].data(), ctg.data(), (long long)rec->pos);
                unknown_allele_total += 1;
                continue;
            }
            if (alt_idx == 0) continue; // nothing to do if reference
            std::string alt = rec->d.allele[alt_idx];

            /* // count unphased variants (once per potentially diploid variant) */
            /* if (!counted_unphased && !bcf_gt_is_phased(gt[hap])) { */
            /*     if (g.verbosity > 1) */
            /*         WARN("Unphased genotype in %s VCF at %s:%lld", */
            /*             callset_strs[callset].data(), ctg.data(), (long long)rec->pos); */
            /*     unphased_gt_total += 1; */
            /*     counted_unphased = true; */
            /* } */

            // skip spanning deletion
            if (alt == "*") { ntypes[hap][TYPE_REF]++; continue; }

            // determine variant type
            int pos = rec->pos;
            int type = -1;
            int lm = 0; // match from left->right (trim prefix)
            int rm = -1;// match from right->left (simplify complex variants CPX->INDEL)
            int reflen = int(ref.size());
            int altlen = int(alt.size());
            if (altlen-reflen > 0) { // insertion
                while (lm < reflen && ref[lm] == alt[lm]) lm++;
                while (reflen+rm >= lm && 
                        ref[reflen+rm] == alt[altlen+rm]) rm--;
                if (lm > reflen+rm) type = TYPE_INS; else type = TYPE_CPX;
                pos += lm;
                alt = alt.substr(lm, altlen+rm-lm+1);
                ref = ref.substr(lm, reflen+rm-lm+1);

            } else if (altlen-reflen < 0) { // deletion
                while (lm < altlen && ref[lm] == alt[lm]) lm++;
                while (altlen+rm >= lm && 
                        ref[reflen+rm] == alt[altlen+rm]) rm--;
                if (lm > altlen+rm) type = TYPE_DEL; else type = TYPE_CPX;
                pos += lm;
                alt = alt.substr(lm, altlen+rm-lm+1);
                ref = ref.substr(lm, reflen+rm-lm+1);

            } else { // substitution
                if (ref.size() == 1) {
                    type = (ref[0] == alt[0] ? TYPE_REF : TYPE_SUB);
                    if (type == TYPE_REF) continue;
                } else {
                    if (ref.substr(1) == alt.substr(1)){
                        type = TYPE_SUB;
                        ref = ref[0]; alt = alt[0]; // chop off matches
                    }
                    else type = TYPE_CPX;
                }
            }

            // calculate reference length of variant
            int rlen = 0;
            switch (type) {
                case TYPE_INS:
                    rlen = 0; break;
                case TYPE_SUB:
                case TYPE_REF:
                    rlen = 1; break;
                case TYPE_DEL:
                case TYPE_CPX:
                    rlen = ref.size(); break;
                default:
                    ERROR("Unexpected variant type: %d", type);
                    break;
            }

            // check that variant is in region of interest
            uint8_t loc = g.bed.contains(ctg, pos, pos + rlen);
            switch (loc) {
                case BED_OUTSIDE: 
                case BED_OFFCTG:
                    nregions[loc]++;
                    continue; // discard variant
                case BED_INSIDE: 
                case BED_BORDER:
                    nregions[loc]++;
                    break;
                default:
                    ERROR("Unexpected BED region type: %d", loc);
                    break;
            }

            // skip variants that are too small or large
            if (int(ref.size()) > g.max_size || int(alt.size()) > g.max_size) {
                if (g.verbosity > 1)
                    WARN("Large variant of length %d in %s VCF at %s:%lld, skipping",
                        int(std::max(ref.size(), alt.size())),
                        callset_strs[callset].data(), ctg.data(), (long long)rec->pos);
                large_var_total++;
                continue;
            }
            if (int(ref.size()) < g.min_size && int(alt.size()) < g.min_size) {
                if (g.verbosity > 1)
                    WARN("Small variant of length %d in %s VCF at %s:%lld, skipping",
                        int(std::max(ref.size(), alt.size())),
                        callset_strs[callset].data(), ctg.data(), (long long)rec->pos);
                small_var_total++;
                continue;
            }

            // TODO: keep overlaps, test all non-overlapping subsets?
            // skip overlapping variants
            if (prev_end[hap] > pos) { // warn if overlap
                if (g.verbosity > 1) {
                    WARN("Overlap in %s VCF variants at %s:%i, skipping", 
                            callset_strs[callset].data(), ctg.data(), pos);
                }
                overlapping_var_total++;
                continue;
            }

            // add to haplotype-specific query info
            if (type == TYPE_CPX) { // split CPX into INS+DEL
                this->ctg_variants[hap][ctg]->add_var(pos, 0, // INS
                    hap, TYPE_INS, loc, "", alt, simple_gt, gq[0], vq);
                this->ctg_variants[hap][ctg]->add_var(pos, rlen, // DEL
                    hap, TYPE_DEL, loc, ref, "", simple_gt, gq[0], vq);
            } else {
                this->ctg_variants[hap][ctg]->add_var(pos, rlen,
                        hap, type, loc, ref, alt, simple_gt, gq[0], vq);
            }

            prev_end[hap] = pos + rlen;
            npass[hap]++;
            ntypes[hap][type]++;
        }
    }

    /* if (gq_missing_total) */ 
    /*     WARN("%d total missing GQ tags in %s VCF, all considered GQ=0", */
    /*         gq_missing_total, callset_strs[callset].data()); */

    if (unknown_allele_total) 
        WARN("%d total unknown alleles (.) found in %s VCF, skipped",
            unknown_allele_total, callset_strs[callset].data());

    if (wrong_ploidy_total) 
        WARN("%d total variants with incorrect ploidy found in %s VCF, kept",
            wrong_ploidy_total, callset_strs[callset].data());

    /* if (unphased_gt_total) */ 
    /*     WARN("%d total unphased genotypes found in %s VCF", */
    /*         unphased_gt_total, callset_strs[callset].data()); */

    if (large_var_total)
        WARN("%d total large %s VCF variant calls skipped, size > %d", 
                large_var_total, callset_strs[callset].data(), g.max_size);

    if (small_var_total)
        WARN("%d total small %s VCF variant calls skipped, size < %d", 
                small_var_total, callset_strs[callset].data(), g.min_size);

    if (overlapping_var_total)
        WARN("%d total overlapping %s VCF variant calls skipped", 
                overlapping_var_total, callset_strs[callset].data());

    if (g.verbosity >= 1) {
        INFO("  Contigs:");
        for (size_t i = 0; i < this->contigs.size(); i++) {
            INFO("    [%2lu] %s: %d | %d variants", i, this->contigs[i].data(),
                    this->ctg_variants[HAP1][this->contigs[i]]->n, 
                    this->ctg_variants[HAP2][this->contigs[i]]->n);
        }
        INFO(" ");

        INFO("  Genotypes:");
        for (size_t i = 0; i < gt_strs.size(); i++) {
            INFO("    %3s  %i", gt_strs[i].data(), ngts[i]);
        }
        INFO(" ");

        INFO("  Variant exceeds min qual (%d):", g.min_qual);
        INFO("    FAIL  %d", pass_min_qual[FAIL]);
        INFO("    PASS  %d", pass_min_qual[PASS]);
        INFO(" ");

        INFO("  Variants in BED regions:");
        for (size_t i = 0; i < region_strs.size(); i++) {
            INFO("    %s  %i", region_strs[i].data(), nregions[i]);
        }
        INFO(" ");

        INFO("  Variant types:");
        if (g.verbosity >= 2) { // show each hap separately
            for (int h = 0; h < HAPS; h++) {
                INFO("    Haplotype %i", h+1);
                for (size_t i = 0; i < type_strs.size(); i++) {
                    INFO("      %s  %i", type_strs[i].data(), ntypes[h][i]);
                }
            }
            INFO(" ");
        } else { // summarize
            for (size_t i = 0; i < type_strs.size(); i++) {
                INFO("    %s  %i", type_strs[i].data(), 
                        ntypes[HAP1][i] + ntypes[HAP2][i]);
            }
        }
        INFO(" ");

        INFO("  %s VCF overview:", callset_strs[callset].data());
        INFO("    TOTAL %d", n);
        INFO("    KEPT  %d", npass[HAP1] + npass[HAP2]);
    }

    free(gq);
    free(fgq);
    free(gt);
    free(ctgnames);
    bcf_hdr_destroy(hdr);
    bcf_close(vcf);
    bcf_destroy(rec);
    return;
error2:
    free(ctgnames);
error1:
    bcf_close(vcf);
    bcf_hdr_destroy(hdr);
    return;

}
