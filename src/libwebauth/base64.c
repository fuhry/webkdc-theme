#include "webauthp.h"

#define XX 127
/*
 * Table for decoding base64
 */
static const unsigned char index_64[256] = {
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, 62, XX, XX, XX, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, XX, XX, XX, XX, XX, XX,
    XX,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, XX, XX, XX, XX, XX,
    XX, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX,
    XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX, XX
};
#define CHAR64(c)  (index_64[(unsigned char)(c)])


static char basis_64[] =
   "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";


int webauth_base64_encoded_length(int length)
{
    assert(length > 0);
    return ((length+2)/3*4);
}

int webauth_base64_encode(const unsigned char *input, 
                          int input_len, 
                          unsigned char *output, 
                          int output_max)
{
    int c1, c2, c3;
    int out_len = 0;

    assert(input != NULL);
    assert(output != NULL);
    assert(input_len > 0);

    while (input_len) {
	c1 = *input++;
	input_len--;

        if (out_len == output_max) return WA_ERR_NO_ROOM;
	output[out_len] = basis_64[c1>>2];
	out_len += 1;

	if (input_len == 0) c2 = 0;
	else c2 = *input++;

        if (out_len == output_max) return WA_ERR_NO_ROOM;
	output[out_len] = basis_64[((c1 & 0x3)<< 4) | ((c2 & 0xF0) >> 4)];
	out_len += 1;

	if (input_len == 0) {
	    output[out_len] = '='; out_len +=1;
	    output[out_len] = '='; out_len +=1;
	    break;
	}

	if (--input_len == 0) c3 = 0;
	else c3 = *input++;

        if (out_len == output_max) return WA_ERR_NO_ROOM;
	output[out_len] = basis_64[((c2 & 0xF) << 2) | ((c3 & 0xC0) >>6)];
	(out_len)++;

	if (input_len == 0) {
	    output[out_len] = '=';
	    out_len +=1;
	    break;
	}
	
	--input_len;
        if (out_len == output_max) return WA_ERR_NO_ROOM;
	output[out_len] = basis_64[c3 & 0x3F];
	out_len += 1;
    }
    
    return(out_len);
}

/*
 * Parse a base64_string
 */

int 
webauth_base64_decode(unsigned char *input,
                      int input_len,
                      unsigned char *output,
                      int output_max)
{
    int c1, c2, c3, c4;
    int i, j;
    int out_len = 0;
    i = 0;
    j = input_len - 4;

    assert(input != NULL);
    assert(input_len >0 && (input_len % 4 == 0));
    assert(output != NULL);

    while (i <= j) {

	c1 = input[i++]; 
        if (CHAR64(c1) == XX) return WA_ERR_CORRUPT;
	c2 = input[i++]; 
        if (CHAR64(c2) == XX) return WA_ERR_CORRUPT;
	c3 = input[i++];
        if (c3 != '=' && CHAR64(c3) == XX) return WA_ERR_CORRUPT;
	c4 = input[i++];
        if (c4 != '=' && CHAR64(c4) == XX) return WA_ERR_CORRUPT;

	if (out_len == output_max) return WA_ERR_NO_ROOM;
	output[(out_len)++] = ((CHAR64(c1)<<2) | ((CHAR64(c2)&0x30)>>4));
	if (c3 == '=') {
	    if (c4 != '=') return WA_ERR_CORRUPT;
	    else return (out_len);
	}
	if (out_len == output_max) return WA_ERR_NO_ROOM;
	output[(out_len)++] = 
	                  (((CHAR64(c2)&0xf)<<4) | ((CHAR64(c3)&0x3c)>>2));

	if (c4 == '=') return (out_len);

	if (out_len == output_max) return WA_ERR_NO_ROOM;
	output[(out_len)++] = (((CHAR64(c3)&0x3)<<6) | CHAR64(c4));
	if (i == input_len) return (out_len);
    }
    return WA_ERR_NO_ROOM;
}
