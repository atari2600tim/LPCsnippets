/*
Some functions for Base64 encoding and decoding
It needs buffer type enabled, maybe some other things.
Use however you like.
Tim@EF, Sep 2016

Basically, Base64 uses printable characters to represent data.
It uses one of 64 characters, so they represent 6 bits each.
A-Z,a-z,0-9,+/
Bytes are 8 bits, so each character holds 3/4 of a byte, like this...
|---------| |---------| |---------| |---------|
1 2 3 4 5 6 1 2 3 4 5 6 1 2 3 4 5 6 1 2 3 4 5 6
1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8 1 2 3 4 5 6 7 8
|-------------| |-------------| |-------------|
*/
#include <driver/runtime_config.h>
string b64table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

string base64_encode(mixed m){
	// Encodes buffer or string into Base64 string.
	int i, j, sz;
	buffer inbuf, outbuf;
	string outstr;
	// First, put the data into a buffer variable even if a string was given just to make code simpler.
	if(stringp(m)){
		sz = sizeof(m);

		// First, check if this will generate a string that is too long.
		i = sz/3*4+(sz%3?4:0); // i=size of final encoded string
		i += i/75; // some room for the newlines
		j = get_config(__MAX_STRING_LENGTH__);
		if(i>j){
			error("base64_encode given string size="+sz+" would create a string of size "+i+"; max is "+j);
		}

		// Check if there is room for string+padding in a buffer.
		i = sz+(3-(sz%3))%3; // Length of input string rounded up to multiple of 3.
		j = get_config(__MAX_BUFFER_SIZE__);
		if(i>j){
			error("base64_encode given string size="+sz+" would use a buffer of size "+i+"; max is "+j);
		}

		inbuf = allocate_buffer(sz+(sz%3?3:0));
		write_buffer(inbuf,0,m);
	}
	else if(bufferp(m)){
		sz=sizeof(m);

		// Check if this will generate a string that is too long.
		i = sz/3*4+(sz%3?4:0); // i=size of final encoded string
		i += i/75; // some room for newlines
		j = get_config(__MAX_STRING_LENGTH__);
		if(i>j){
			error("base64_encode given buffer size="+sz+" would create a string of size "+i+"; max is "+j);
		}

		// Check if there is room for padding in a buffer, then copy the data.
		i = sz+(3-(sz%3))%3; // How much to add to make it a multiple of 3.  Probably a simpler way.
		j = get_config(__MAX_BUFFER_SIZE__);
		if(i>j){
			error("base64_encode given buffer size="+sz+" would use a buffer of size "+i+"; max is "+j);
		}
		inbuf=copy(m)+allocate_buffer(i);

	}
	else {
		error("base64_encode was given arg of type "+typeof(m));
	}
	// Now inbuf holds copy of the original data, plus maybe some padding.

	// Next, make an output buffer of 4/3 size.
	// If input was not a multiple of 3 then need to add some to make it a multiple of 4.
	outbuf = allocate_buffer(sz/3*4+(sz%3?4:0));

	// Split the data 3->4.
	for(i=0;i<sz;i+=3){
		j = i/3*4;
		outbuf[j]=inbuf[i]/4;
		outbuf[j+1]=((inbuf[i]%4)*16)+(inbuf[i+1]/16);
		outbuf[j+2]=((inbuf[i+1]%16)*4)+(inbuf[i+2]/64);
		outbuf[j+3]=inbuf[i+2]%64;
	}

	// Next, change output values to character values so that it can be changed to string soon.
	j = sz/3*4+(sz%3?4:0);
	for(i=0;i<j;i++){
		outbuf[i] = b64table[outbuf[i]];
	}

	// Handle padding.
	if((sz%3)==2){
		outbuf[j-1]='=';
	}
	else if((sz%3)==1){
		outbuf[j-1]='=';
		outbuf[j-2]='=';
	}

	// Switch output buffer to string.
	outstr = read_buffer(outbuf,0);

	// At this point, you could throw in some newlines so that lines aren't long.  I think this is optional.
	// suggestions say to keep it under 76 characters
#if efun_defined(break_string)
// Not sure when this was introduced but it is not on FluffOS 2.16; won't bother writing alternate code for older.
	outstr = break_string(outstr,75);
#endif
	return outstr;
}

mixed base64_decode(string str){
	// Decodes encoded string into decoded data
	// If there is an error, return 0
	// Return data as either string or buffer, depending on whether it contains a null or extended ASCII.
	int i,j,k,l,m,n;
	string instr;
	buffer outbuf;
	int in_sz,zero_found=0;

	// Strip white space, should be just newlines I think.
	instr = replace_string(str,"\n","");
        instr = replace_string(instr," ","");
        instr = replace_string(instr,"\t","");
        instr = replace_string(instr,"\r","");

	// Input should be blocks of 4 characters.
	in_sz = sizeof(instr);
	if(in_sz%4) error("base64_decode input should be multiple of 4 but instead is size "+in_sz);

	// Make sure buffers can hold this much data.
	i = in_sz/4*3;
	j = get_config(__MAX_BUFFER_SIZE__);
	if(i>j){
		error("input string size="+in_sz+" would create buffer size="+i+"; max is "+j);
	}

	// Allocate buffer for output.  Smaller because decoding has 4 inputs and 3 outputs.
	outbuf = allocate_buffer(i);

	// Handle each block.
	for(i=0;i<sizeof(instr);i+=4){
		// Get the 4 inputs...
		j = member_array(instr[i],b64table);
		k = member_array(instr[i+1],b64table);
		l = member_array(instr[i+2],b64table);
		m = member_array(instr[i+3],b64table);
		// Convert them to 3 outputs...
		n = i/4*3;
		outbuf[n] = j*4 + k/16;
		outbuf[n+1] = (k%16)*16 + l/4;
		outbuf[n+2] = (l%4)*64 + m%64;
	}

	// Figure out result size if there was padding.
	if(instr[in_sz-2]=='='){ j = in_sz/4*3-2; }
	else if(instr[in_sz-1]=='='){ j = in_sz/4*3-1; }
	else { j = in_sz/4*3; }

	// Decide if we should return a string.
	for(i=0;i<j;i++){
		if((outbuf[i]==0) || (outbuf[i]>127) ){ zero_found=1; }
	}

	// return a string or buffer
	if(zero_found) return outbuf[0..j];
	return read_buffer(outbuf,0,j);
}
