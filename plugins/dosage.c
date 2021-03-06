#include <stdio.h>
#include <stdlib.h>
#include <htslib/vcf.h>
#include <math.h>
#include "config.h"


/* 
    This short description is used to generate the output of `bcftools annotate -l`.
*/
const char *about(void)
{
    return 
        "Prints genotype dosage determined from tags requested by the user.\n"
        "By default the plugin searches for PL, GL and GT (in that order), thus\n"
        "running with \"-p dosage\" is equivalent to \"-p dosage:tags=PL,GL,GT\".\n";
}


bcf_hdr_t *in_hdr = NULL;
int pl_type = 0, gl_type = 0;
uint8_t *buf = NULL;
int nbuf = 0;   // NB: number of elements, not bytes
char **tags = NULL;
int ntags = 0;

typedef int (*dosage_f) (bcf1_t *);
dosage_f *handlers = NULL;
int nhandlers = 0;


int calc_dosage_PL(bcf1_t *rec)
{
    int i, j, nret = bcf_get_format_values(in_hdr,rec,"PL",(void**)&buf,&nbuf,pl_type);
    if ( nret<0 ) return -1;

    nret /= rec->n_sample;
    #define BRANCH(type_t,is_missing,is_vector_end) \
    { \
        type_t *ptr = (type_t*) buf; \
        for (i=0; i<rec->n_sample; i++) \
        { \
            float vals[3] = {0,0,0}; \
            for (j=0; j<nret; j++) \
            { \
                if ( is_missing || is_vector_end ) break; \
                vals[j] = exp(-0.1*ptr[j]); \
            } \
            float sum = vals[0] + vals[1] + vals[2]; \
            printf("\t%.1f", sum==0 ? -1 : (vals[1] + 2*vals[2]) / sum); \
            ptr += nret; \
        } \
    }
    switch (pl_type)
    {
        case BCF_HT_INT:  BRANCH(int32_t,ptr[j]==bcf_int32_missing,ptr[j]==bcf_int32_vector_end); break;
        case BCF_HT_REAL: BRANCH(float,bcf_float_is_missing(ptr[j]),bcf_float_is_vector_end(ptr[j])); break;
    }
    #undef BRANCH
    return 0;
}

int calc_dosage_GL(bcf1_t *rec)
{
    int i, j, nret = bcf_get_format_values(in_hdr,rec,"GL",(void**)&buf,&nbuf,pl_type);
    if ( nret<0 ) return -1;

    nret /= rec->n_sample;
    #define BRANCH(type_t,is_missing,is_vector_end) \
    { \
        type_t *ptr = (type_t*) buf; \
        for (i=0; i<rec->n_sample; i++) \
        { \
            float vals[3] = {0,0,0}; \
            for (j=0; j<nret; j++) \
            { \
                if ( is_missing || is_vector_end ) break; \
                vals[j] = exp(ptr[j]); \
            } \
            float sum = vals[0] + vals[1] + vals[2]; \
            printf("\t%.1f", sum==0 ? -1 : (vals[1] + 2*vals[2]) / sum); \
            ptr  += nret; \
        } \
    }
    switch (pl_type)
    {
        case BCF_HT_INT:  BRANCH(int32_t,ptr[j]==bcf_int32_missing,ptr[j]==bcf_int32_vector_end); break;
        case BCF_HT_REAL: BRANCH(float,bcf_float_is_missing(ptr[j]),bcf_float_is_vector_end(ptr[j])); break;
    }
    #undef BRANCH
    return 0;
}

int calc_dosage_GT(bcf1_t *rec)
{
    int i, j, nret = bcf_get_genotypes(in_hdr,rec,(void**)&buf,&nbuf);
    if ( nret<0 ) return -1;

    nret /= rec->n_sample;
    int32_t *ptr = (int32_t*) buf;
    for (i=0; i<rec->n_sample; i++)
    { 
        float dsg = 0;
        for (j=0; j<nret; j++)
        { 
            if ( ptr[j]==bcf_int32_missing || ptr[j]==bcf_int32_vector_end || ptr[j]==bcf_gt_missing ) break;
            if ( bcf_gt_allele(ptr[j]) ) dsg += 1;
        }
        printf("\t%.1f", j>0 ? dsg : -1);
        ptr += nret;
    }
    return 0;
}



/* 
    Called once at startup, allows to initialize local variables.
    Return 1 to suppress VCF/BCF header from printing, 0 for standard
    VCF/BCF output and -1 on critical errors.
*/
int init(const char *opts, bcf_hdr_t *in, bcf_hdr_t *out)
{
    int i, id;

    in_hdr = in;
    tags = config_get_list(opts ? opts : "tags=PL,GL,GT","tags", &ntags);
    for (i=0; i<ntags; i++)
    {
        if ( !strcmp("PL",tags[i]) )
        {
            id = bcf_hdr_id2int(in_hdr,BCF_DT_ID,"PL");
            if ( bcf_hdr_idinfo_exists(in_hdr,BCF_HL_FMT,id) ) 
            {
                pl_type = bcf_hdr_id2type(in_hdr,BCF_HL_FMT,id);
                if ( pl_type!=BCF_HT_INT && pl_type!=BCF_HT_REAL ) 
                {
                    fprintf(stderr,"Expected numeric type of FORMAT/PL\n");
                    return -1;
                }
                handlers = (dosage_f*) realloc(handlers,(nhandlers+1)*sizeof(*handlers));
                handlers[nhandlers++] = calc_dosage_PL;
            }
        }
        else if ( !strcmp("GL",tags[i]) )
        {
            id = bcf_hdr_id2int(in_hdr,BCF_DT_ID,"GL");
            if ( bcf_hdr_idinfo_exists(in_hdr,BCF_HL_FMT,id) )
            {
                gl_type = bcf_hdr_id2type(in_hdr,BCF_HL_FMT,id);
                if ( gl_type!=BCF_HT_INT && gl_type!=BCF_HT_REAL ) 
                {
                    fprintf(stderr,"Expected numeric type of FORMAT/GL\n");
                    return -1;
                }
                handlers = (dosage_f*) realloc(handlers,(nhandlers+1)*sizeof(*handlers));
                handlers[nhandlers++] = calc_dosage_GL;
            }
        }
        else if ( !strcmp("GT",tags[i]) )
        {
            handlers = (dosage_f*) realloc(handlers,(nhandlers+1)*sizeof(*handlers));
            handlers[nhandlers++] = calc_dosage_GT;
        }
        else 
        {
            fprintf(stderr,"No handler for tag \"%s\"\n", tags[i]);
            return -1;
        }
    }
    free(tags[0]);
    free(tags);

    printf("#[1]CHROM\t[2]POS\t[3]REF\t[4]ALT");
    for (i=0; i<bcf_hdr_nsamples(in_hdr); i++) printf("\t[%d]%s", i+5,in_hdr->samples[i]);
    printf("\n");

    return 1;
}


/*
    Called for each VCF record after all standard annotation things are finished.
    Return 0 on success, 1 to suppress the line from printing, -1 on critical errors.
*/
int process(bcf1_t *rec)
{
    int i, ret;

    printf("%s\t%d\t%s\t%s", bcf_seqname(in_hdr,rec),rec->pos+1,rec->d.allele[0],rec->n_allele>1 ? rec->d.allele[1] : ".");
    if ( rec->n_allele==1 )
    {
        for (i=0; i<rec->n_sample; i++) printf("\t0.0");
    }
    else
    {
        for (i=0; i<nhandlers; i++)
        {
            ret = handlers[i](rec);
            if ( !ret ) break;  // successfully printed
        }
        if ( i==nhandlers )
        {
            // none of the annotations present
            for (i=0; i<rec->n_sample; i++) printf("\t-1.0");
        }
    }
    printf("\n");

    return 1;
}


/*
    Clean up.
*/
void destroy(void)
{
    free(handlers);
    free(buf);
}


