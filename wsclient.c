/* wsclient.c
 This is an object to act as a websocket client and maybe enable other objects to do websocket things
 The WebSocket Protocol: https://tools.ietf.org/html/rfc6455
 Writing WebSocket servers: https://developer.mozilla.org/en-US/docs/Web/API/WebSockets_API/Writing_WebSocket_servers
 I'll try to make it relatively generic for reuse and sharing with the public and all.
 -Tim, Jan 2019

 Feb 25 & 26: Seems to have worked alright for a while, so finishing it up a bit now and separating all the parts
 Told the Grapevine people that I would share this soon, and I've been stable on there for a week now
 Cut way back on debugging messages, changed some #define properties to variables with a setup function
 Reduced and renamed functions
 */

/* How to use this...
   You need a base64_encode simul_efun that encodes buffers like mine at https://www.eternalfantasy.org/home/tim/code/base64.txt
   You need CRYTPO package for hash("sha1",string).  There are other driver configuration things I should make note of later.
   It uses the valid_socket master apply to check whether the calling object has permission to connect to the remote site, so set up access for both this and your thing controlling this.

 Build an object that you will use to control this.  Have it clone and configure this.  You will need these functions for it to call (name them whatever you want):
   close_callback(string) is the name of a function in your object that notifies you when disconnected, string argument gives explanation
     this is called when network stops responding, when either side decides to close connection, or any other reason
   read_callback(string|buffer) is function in your object informing you of incoming messages
   online_callback() is how you want to be notified you that it has successfully connected and finished the handshake

 Here are functions that your thing will want to call in this websocket client object:
 send(string|buffer)
   That will send a message to the server, it will use opcode indicating text if you send a string, or binary if you send a buffer, unless you override it with set_datatype
 drop_connection(string)
   That will send a CLOSE packet to the server and close the connection.
 void set_datatype(int) ---optional, do it before setup_websocket
    I connected to servers who use text and they use opcode marking their text messages as "binary" even though they are all text
    The default behavior I wrote is to look at opcode of message and then send you strings or buffers based on that opcode, so your callback would have to accept both
    Use this to override that and declare that you want for your read_callback function to be given strings with outgoing messages using opcode binary, or whatever combination.
      send to your read_callback based on opcode (A), always string (B), always buffer(C) by adding 0, 1 or 2
      send to server using opcode based on type of variable you sent (X), always text(Y), always binary(Z) by adding 0, 4 or 8
            A  B  C
         X  0  1  2
         Y  4  5  6
         Z  8  9  10
 void set_subprotocol(string) ---optional, do it before setup_websocket
   Some servers want you to specify a protocol.  When I set up Websockify to act as websocket-to-telnet proxy, I had to tell it "binary".
   Servers can use whatever names they want, and people suggest to use something unique
   Default behavior I did is to leave out the Sec-Websocket-Protocol line, but you can use this to add that line
 string setup_websocket("127.0.0.1 2000", "example.com", "/testmud", "close_callback", "read_callback", "online_callback");
   That will return a string with error message, or 0 if success.  Here are the arguments:
    string with IP address, a space, and port to connect to, it does not have to match the real host (for example you might have a local TLS proxy to a wss: server)
    string host name that you will be requesting
    string path that you will request
    string close_callback - name of function in your object
    string read_callback - name of function in your object
    string online_callback - name of function in your object
 */

/* NOTES:
 Outgoing messages are all sent with one frame and every frame has the final bit set on it.
 It understands incoming messages that are split up just fine though.
 If you want to send large pieces of data and queue things, throttle data usage or whatever, this would need some changes.
 I'm not even sure how one would queue things, I assume you'd send a piece together with a heartbeat and then wait on reply?
 I'll figure it out after I find a websocket file server or something I want to interact with I guess.

 This is set up to use a known IP... if you have domain but not IP then you'll want to resolve it first.
 Wouldn't be hard to add it on here though maybe in future revision.

 TODO:
 If you want strings and it gives you binary data then I should watch for that, as strings can't have null in middle, probably causes errors at the moment.
 I'll find out after I try bigger variety of servers.
 It is missing all kinds of sanity checks.
 A handshake reply that deviates even a tiny bit gets rejected, even if they are likely valid.  I'm wanting to see how much different servers differ.
 */

/*Frame format copied and pasted from the RFC:
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-------+-+-------------+-------------------------------+
   |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
   |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
   |N|V|V|V|       |S|             |   (if payload len==126/127)   |
   | |1|2|3|       |K|             |                               |
   +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
   |     Extended payload length continued, if payload len == 127  |
   + - - - - - - - - - - - - - - - +-------------------------------+
   |                               |Masking-key, if MASK set to 1  |
   +-------------------------------+-------------------------------+
   | Masking-key (continued)       |          Payload Data         |
   +-------------------------------- - - - - - - - - - - - - - - - +
   :                     Payload Data continued ...                :
   + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
   |                     Payload Data continued ...                |
   +---------------------------------------------------------------+

     The opcode field defines how to interpret the payload data: 0x0 for continuation,
     0x1 for text (which is always encoded in UTF-8), 0x2 for binary, and other so-called
     "control codes" that will be discussed later. In this version of WebSockets,
     0x3 to 0x7 and 0xB to 0xF have no meaning.
 */

#include <net/socket.h>
#include <driver/runtime_config.h>

// high bit of opcode indicates control(1) or data(0), so 0-7 are data, 8-F are control
#define OPCODE_CONTINUATION 0x0
#define OPCODE_TEXT 0x1
#define OPCODE_BINARY 0x2
// 3-7 reserved for data frames yet to be defined
#define OPCODE_CLOSE 0x8
#define OPCODE_PING 0x9
#define OPCODE_PONG 0xA
// B-F reserved for control frames yet to be defined

string ws_server; // string formatted for socket_connect with ip,space,port like "127.0.0.1 2000"
// It is where you connect to, and does not have to match URI, for example it could be stunnel on localhost (meaning this can use wss: even though it does not handle TLS)
// scheme://host:port/path
string host; // like "grapevine.haus", used in handshake for virtual host on Host: line
string path; // like "/socket" or "/game", used in handshake for GET: line
object callback_object; // object that is using this
string close_cb; // string or function to be called when closed connection
string read_cb; // string or function used for callback
string online_cb; // callback to notify that you've done the handshake
string subprotocol;
int usetype=0;

// open connection variables
int socket; // socket ID
buffer net_buf; // incoming bytes that have not been handled yet
buffer msg_buf; // incoming payload from data frames
int opcode_buf; // opcode saved from first frame in message
int connected; // got successful handshake
string ws_key; // key that we generated to use for handshake
string ws_key_reply; // valid reply for above key

void debug(string str);
string setup_websocket(string a, string h, string p, string ccb, string rcb, string ocb);
void read_callback(int s,buffer b);
void write_callback(int s);
void close_callback(int s);
void send_handshake();
void handle_handshake();
void handle_frame();
void handle_data();
void send_frame(buffer b, int opcode);
void send(mixed arg);
void drop_connection(string str);
void set_datatype(int i);
void set_subprotocol(string str);
void remove();

void debug(string str){
  int i;
  if(sscanf(file_name(this_object()),"%*s#%d",i)==0) i=0;
  foreach(object ob in users()){
    if(adminp(ob) && ob->query_env("debugWS")) tell_object(ob, "WSdebug["+i+"]: "+str+"\n");
  }
  log_file("adm/ws.debug","#"+i+" "+time()+": "+str+"\n");
 }

string setup_websocket(string a, string h, string p, string ccb, string rcb, string ocb) { 
  // Return a string with error message, or else 0 for success
  // (address, host, path, close_callback, read_callback, online_callback, usetype)
  // usetype... 1 means read_callback expects a string, 2 means buffer, 0 means use opcode to decide
  int result,i,j; object caller; buffer b; string str;
  if(!clonep(this_object())) return "only use clones of the websocket client object";
  seteuid(getuid()); // I believe is required for permission to do socket efuns
  caller = previous_object();
  debug(sprintf("setup called, caller=%O, a=%s, h=%s, p=%s, ccb=%s, rcb=%s, ocb=%s",caller,a,h,p,ccb,rcb,ocb));
  // TODO: Check master object and require that caller has access to use socket functions itself.
  // If you want to allow something without changing the master object function then put your exception here
  sscanf(a,"%s %d",str,i);
  j = master()->valid_socket(caller, "create", ({0,caller,str,i})); // ask master object if the caller can connect there, if not then we won't on their behalf
  if(!j){
    return sprintf("Master object says that %O does not have access to connect to %s:%d",caller,str,i);
  }
  if(ws_server){
    if(connected) return "this object is already connected";
    else return "this object is already trying to connect";
  }
  callback_object = caller;
  ws_server = a;
  host = h;
  path = p;
  close_cb = ccb;
  read_cb = rcb;
  online_cb = ocb;
  net_buf = allocate_buffer(0);
  msg_buf = allocate_buffer(0);
  socket = 0;
  connected = 0;
  b = allocate_buffer(16);
  for(i=0;i<16;i++){
    b[i]=random(256);
  }
  ws_key = base64_encode(b);
  str = hash("sha1",ws_key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  b=allocate_buffer(20);
  for(i=0;i<20;i++){ // base64(sha1(key+magicnumber)) calculates based on values represented by the sha1 rather than the string
    sscanf(str[2*i..2*i+1],"%x",j);
    b[i] = j;
  }  
  ws_key_reply = base64_encode(b);
  result = socket_create(STREAM_BINARY, "read_callback", "close_callback");
  // I -think- the read_callback here is only used for UDP messages, while TCP messages use the read_callback passed to socket_connect.
  if(result<0){
    debug("socket_create error: "+socket_error(result));
    return "error with socket_create: "+socket_error(result);
  }
  debug("created socket descriptor "+result);
  socket = result;
  result = socket_connect(socket, ws_server, "read_callback", "write_callback");
  if (result != EESUCCESS) {
    debug("socket_connect: " + socket_error(result));
    socket_close(socket);
    return "error with socket_connect: "+socket_error(result);
  }
  debug("finished setting up websocket");
  return 0;
 }
void read_callback(int s, buffer b){
  debug("read_callback["+s+"] called, data came in");
  if(origin() != "internal") { debug("halt read_callback, origin should be internal but was "+origin()); return; }
  if(!callback_object) {
    debug("dropping connection because controller object is gone");
    drop_connection("Controller object is unloaded, so dropping connection");
    return; 
  }
  net_buf += b;
  debug("  got data of size ["+sizeof(b)+"], adding to network buffer which is now size ["+sizeof(net_buf)+"]");
  if(!connected){ // if have not completed the handshake process, see if they sent me a full handshake reply
    debug("  not fully connected, so will try handle_handshake()");
    handle_handshake();
  }
  if(!connected){ // maybe handshake reply was split between packets even though they are so small? just hold buffer
    debug("  still not connected even after trying handle_handshake(), but maybe the reply was split between network packets, will hold in net buffer");
    return;
  }
  // TODO: maybe should give up if unreasonable amount of data has come in without recognized response
  // at this point, we have fully connected and got a good handshake
  debug("  we are fully connected, so will try handle_frame()");
  handle_frame();
 }
void write_callback(int s) {
  debug("write_callback["+s+"] called, okay to send data, will send handshake");
  if(origin() != "internal") { debug("halt write_callback, origin should be internal but was "+origin()); return; }
  send_handshake();
 }
void close_callback(int s) {
  debug("close_callback["+s+"] called, disconnected");
  if(origin() != "internal") { debug("halt close_callback, origin should be internal but was "+origin()); return; }
  if(callback_object) call_other(callback_object, close_cb,"lost connection to server");
 }
void send_handshake() {
  buffer b;
  string str;
  debug("send_handshake() called");
  if(origin() != "local") { debug("halt send_handshake, origin should be local but was "+origin()); return; }
  str = sprintf("GET %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "%s"
    "\r\n", path, host, ws_key, (subprotocol ? "Sec-Websocket-Protocol: "+subprotocol+"\r\n": "")
  );  
  b = allocate_buffer(sizeof(str));
  write_buffer(b,0,str);
  socket_write(socket, b);
 }
void handle_handshake() {
  /*
  Look for handshake response, and if it is there then handle it (set connected=1 and whatever else)
  preserve rest of buffer, in case the server decided to immediately say hello
 
  Server Handshake Reponse should look something like this, with \r\n after each line and then doubled at end...
  HTTP/1.1 101 Switching Protocols
  Upgrade: websocket
  Connection: Upgrade
  Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=

  if you listed some protocols that you accept then the server might reply selecting one
  Sec-WebSocket-Protocol: chat

  TODO: see if those are always in same order and if there are any optional things that might be thrown in that we'd care about
  */
  string accept_key, str, *strs,lc;
  int sz;
  int flags=0; // 1=right key, 2=switching, 4=upgrade, 8=connection   15=all good
  debug("handle_handshake() called");
  if(origin() != "local") { debug("halt handle_handshake, origin should be local but was "+origin()); return; }
  sz = sizeof(net_buf);
  if(sz<5){
    debug("   stopping because it's not big enough to contain anything plus CRLFx2, sz="+sz);
    return;
  }
  if(!callback_object){ drop_connection("Started to handle handshake, but callback object is gone so dropping instead"); return; }
  // look for \r\n\r\n
  for(int i=0;i<(sz-3);i++) { // loop stops before the end because we will look ahead to next 3
    if(net_buf[i]==0){ // sent us a null too soon
      debug("we got an incoming null byte while I was looking for handshake response, will just close the socket");
      drop_connection("got incoming null byte while looking for handshake response");
      return;
    }
    if( (net_buf[i] == '\r') && (net_buf[i+1] == '\n') && (net_buf[i+2] == '\r') && (net_buf[i+3] == '\n') ){
      debug("found the CRLF,CRLF... ["+i+"] bytes in");
      // i is location of \r\n\r\n, skip those 4, and str=prior, net_buf=after
      str = read_buffer(net_buf,0,i);
      net_buf = net_buf[(i+4)..(sz-1)];
      strs = explode(str,"\r\n");
      debug(sprintf("  this is indeed the handshake, here are the lines [%s]",save_variable(strs)));
      foreach(str in strs){
        debug(sprintf("    inside the foreach loop, with %s",str));
        /* Here I will verify that the server responded with something that looks like success.
         FluffOS native websocket server has a minimal response, and Apache reverse proxy to it seems to do the same:
          HTTP/1.1 101 Switching Protocols
          Upgrade: websocket
          Connection: Upgrade
          Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
         Websockify requires you to ask for the protocol, and 'binary' is good:
          HTTP/1.1 101 Switching Protocols
          Server: WebSockify Python/2.7.13
          Date: Thu, 31 Jan 2019 00:06:56 GMT
          Upgrade: websocket
          Connection: Upgrade
          Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
          Sec-WebSocket-Protocol: binary
         Grapevine server responds like this:
          HTTP/1.1 101 Switching Protocols
          connection: Upgrade
          date: Fri, 01 Feb 2019 18:12:52 GMT
          sec-websocket-accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
          server: Cowboy
          upgrade: websocket
        */
        lc = lower_case(str);
        if(sscanf(lc,"sec-websocket-accept: %*s") == 1){
          if(str[22..] == ws_key_reply){
            debug("it gave us correct key response");
            flags = flags | 1;
          }
          else{
            debug(sprintf("dropping, server sent us Sec-WebSocket-Accept: %s, we wanted %s", str[22..], ws_key_reply));
            drop_connection(sprintf("Dropping connection, server sent us Sec-WebSocket-Accept: %s, we wanted %s", str[22..], ws_key_reply));
            return;
          }
        }
        if(lc == "http/1.1 101 switching protocols") flags = flags | 2;
        if(lc == "upgrade: websocket") flags = flags | 4;
        if(lc == "connection: upgrade") flags = flags | 8;
      }
      // These are looking for basically exact matches and reject some okay responses, so skip this part if you want.
      if(!(flags&1)){ drop_connection("Dropping, wanted Sec-WebSocket-Accept"); return; }
      if(!(flags&2)){ drop_connection("Dropping, wanted HTTP/1.1 101 Switching Protocols"); return; }
      if(!(flags&4)){ drop_connection("Dropping, wanted Upgrade: websocket"); return; }
      if(!(flags&8)){ drop_connection("Dropping, wanted Connection: Upgrade"); return; }
      connected = 1;
      call_other(callback_object, online_cb);
      return;
    }
  }
  // did not find end of handshake yet, maybe I should drop connection if I got too many packets without identifying a handshake response
  return;
 }
void handle_frame() {
  /*
  Much like handle_handshake, handle_frame will look at the size of buffer, and look at the first few
  bytes to see if they match the websocket container thing and if we've received the whole frame.

  If they do then identify the payload, decode the payload, and append it to an incoming message buffer.

  If it's a control frame then do whatever and take it back off and preserve the rest of the message buffer,
  because a server is supposed to be able to send you control frames in the middle of a message that was
  spread out over a set of data frames.

  The servers I tested on all do frames with the FIN bit on every frame as far as I noticed,
  and haven't gotten any control frames.  FluffOS's thing splits frames at 125, but I got websockify
  to send me a frame of payload size 2589 without splitting it up.
  I made a spam command that sends you about 5k of data, and websockify split it into 1496+2600+929 each having the FIN bit
  Anyway, I need to verify that continuation things work properly, that element is completely untested and I'm almost finished with writing this.

  If this frame has the FINal bit on, then call handle_data().
  */
  int buf_sz,msg_sz,payload_size,payload_start,fin,rsv,opcode;
  string str;
  buffer mask;
  buf_sz = sizeof(net_buf);
  debug("handle_frame() called, while sizeof net_buf is "+buf_sz);
  if(origin() != "local") { debug("halt handle_frame, origin should be local but was "+origin()); return; }
  if(buf_sz<2){
    debug("  not even 2 bytes in network data buffer, stopping");
    return;
  }
  mask = allocate_buffer(4); // initialized to 0's
  payload_size = (net_buf[1] & 127); // payload size is held in last 7 bits of second byte
  // up to 125 means that this is the size of data
  // 126 is special value meaning use next 2 bytes for size, unsigned 16-bit int
  // 127 means use the next 4 bytes for size, unsigned 64-bit int
  // for each scenario, check that you have enough data, then figure out what mask is and index of payload
  // if buffer is smaller than data+header then return and wait for next packet
  if(payload_size < 126){
    if(net_buf[1] & 128){ // if MASK bit is on
      if(buf_sz < (payload_size+6)){ debug("  smaller than data+6"); return; }
      mask = net_buf[2..5];
      payload_start=6;
    }
    else{
      if(buf_sz < (payload_size+2)){ debug("  smalller than data+2"); return; }
      payload_start=2;
    }
  }
  else if(payload_size == 126){
    payload_size = net_buf[2]*0x100 + net_buf[3];
    if(net_buf[1] & 128){ // if MASK bit is on
      if(buf_sz < (payload_size+8)){ debug("  smalller than data+8"); return; }
      mask = net_buf[6..9];
      payload_start=8;
    }
    else{
      if(buf_sz < (payload_size+4)){ debug("  smalller than data+4"); return; }
      payload_start=4;
    }
  }
  else if(payload_size == 127){
    payload_size = net_buf[2]*0x100000000000000 + net_buf[3]*0x1000000000000 + net_buf[4]*0x10000000000 + net_buf[5]*0x100000000 + net_buf[6]*0x1000000 + net_buf[7]*0x10000 + net_buf[8]*0x100 + net_buf[9];
    if(net_buf[1] & 128){ // if MASK bit is on
      if(buf_sz < (payload_size+14)){ debug("  smaller than data+14"); return; }
      mask = net_buf[10..13];
      payload_start=14;
    }
    else{
      if(buf_sz < (payload_size+10)){ debug("  smaller than data+10"); return; }
      payload_start=10;
    }
  }
  debug(sprintf("  mask is %02x %02x %02x %02x, payload_size=%d, payload_start is %d",mask[0],mask[1],mask[2],mask[3],payload_size,payload_start));
  if(payload_size<0){ // Size is unsigned, but 63 bits is huge, should never see 1 on first bit of size.
    debug("  sanity check: negative payload size");
    drop_connection("Dropping connection, unreasonable incoming data frame size");
    return;
  }
  if((payload_size + sizeof(msg_buf)) > get_config(__MAX_BUFFER_SIZE__)){
    debug("  payload added to msg buffer is larger than max buffer size, dropping connection");
    drop_connection("Dropping connection, incoming data frame size "+payload_size+", queued messages are "+sizeof(msg_buf)+", max buffer size is "+get_config(__MAX_BUFFER_SIZE__));
    return;
  }
  // first byte bits are FIN (1), RSV (3), opcode (4)
  fin = net_buf[0]/128;
  rsv = net_buf[0]/16 & 7;
  opcode = net_buf[0] & 15;
  debug(sprintf("  first byte says FIN:%x, RSV:%x, opcode is: %x",fin,rsv,opcode)); // incoming messages from testmud have this as 0x82 (130 decimal) which means FIN=1, RSV=000, opcode=0010 (binary)
  // Now to add the payload to msg_buf, remove it from net_buf, and then decode what we just added to msg_buf.
  msg_sz = sizeof(msg_buf); // msg_sz is now size of message buffer before this was added
  msg_buf += net_buf[payload_start..(payload_start+payload_size-1)];
  net_buf = net_buf[(payload_start+payload_size)..];
  for(int i=0;i<payload_size;i++){
    msg_buf[msg_sz+i] = msg_buf[msg_sz+i] ^ mask[i%4];
  }
  if(opcode & 8){ // the high bit of a 4-bit opcode means it is a control frame
    // Oh, that one was a control frame.  Take it back off of the message buffer and do whatever.
    if(opcode == OPCODE_PING){ // ping has max payload length of 125, they want a PONG with same payload
      debug("   opcode was PING, so sending PONG back");
      send_frame(msg_buf[msg_sz..], OPCODE_PONG);
      msg_buf = msg_buf[0..(msg_sz-1)]; // remove it from msg_buf but keep whatever was in front of it
      return;
    }
    else if(opcode == OPCODE_PONG){
      debug("   opcode was PONG? either side can send a ping or pong, but I did not program this to send a ping");
      msg_buf = msg_buf[0..(msg_sz-1)]; // remove it from msg_buf but keep whatever was in front of it
      return;
    }
    else if(opcode == OPCODE_CLOSE){
      debug("   opcode was CLOSE, send a close back, and disconnect");
      send_frame(allocate_buffer(0),OPCODE_CLOSE);
      // a CLOSE frame may have a payload, it may have 2 bytes for a value and then may also be followed by UTF-8 data
      if(payload_size>2){
        debug(sprintf("       the CLOSE message gave us val=%d msg=%s",msg_buf[0]*256+msg_buf[1],read_buffer(msg_buf,2)));
        drop_connection(sprintf("Server sent us a CLOSE websocket message, value=%d msg=%s",msg_buf[0]*256+msg_buf[1],read_buffer(msg_buf,2)));
      }
      else if(payload_size==2){
        debug(sprintf("       the CLOSE message came with value only, val=%d",msg_buf[0]*256+msg_buf[1]));
        drop_connection(sprintf("Server sent us a CLOSE websocket message with value=%d",msg_buf[0]*256+msg_buf[1]));
      }
      else{
        debug("    the CLOSE message came with no payload");
        drop_connection("Server sent us a CLOSE websocket message with no value or message");
      }
      return;
    }
    else{
      debug("   got an unknown control opcode: "+opcode);
      return;
    }
  }
  debug("   it is not a control frame, so handle the data");
  if(opcode != OPCODE_CONTINUATION){
    opcode_buf = opcode;
  }
  // If it is the final or only message in series, then also handle the message buffer.
  if(fin){
    debug("   final flag was on that, so calling handle_data()");
    handle_data();
  }
  else{
    debug("   final flag was not set");
  }
  // Did they send two frames that both arrived in same packet?
  debug("I am at the end of handle_frame, size of net_buf is "+sizeof(net_buf));
  if(sizeof(net_buf)>1){
     debug("   there is more data, did they send 2 frames that both arrived in same packet? will call handle_frame() again");
     handle_frame();
  }
 }
void handle_data()
 {
  /*
  This handles the data stored in the incoming message buffer.
  It should be a full message (last one was marked with FIN bit) when this is called.
  */
  int err;
  int t;
  debug("handle_data called, msg_buf is size "+sizeof(msg_buf)+", which I am deleting for now, and opcode is "+opcode_buf);
  debug(" the data is:"+identify(msg_buf));
  if(origin() != "local") { debug("halt handle_data, origin should be local but was "+origin()); return; }
  if(usetype & 3){ t = usetype & 3; }
  else{
    if(opcode_buf==OPCODE_TEXT) t=1;
    if(opcode_buf==OPCODE_BINARY) t=2;
  }
  err = catch {
    if(t==1){
      debug("  doing the callback with string");
      call_other(callback_object,read_cb,read_buffer(msg_buf));
    }
    if(t==2){
      debug("  doing callback with buffer");
      call_other(callback_object,read_cb,msg_buf);
    }
  };
  if(err) debug(sprintf("CATCH_ERROR handling packet, opcode=%d, t=%d msg_buf=%s",opcode_buf,t,identify(msg_buf)));
  msg_buf = allocate_buffer(0);
 }
void send_frame(buffer b, int opcode)
 { // Take given data, build a websocket frame for it, and send that out to the server
  buffer to_send;
  buffer mask;
  int sz, payload_start;
  debug("send_data called, typeof="+typeof(b)+", opcode="+opcode);
  if(origin() != "local") { debug("halt send_frame, origin should be local but was "+origin()); return; }
  if(!connected){
    debug("  not connected though, stopping here");
    return;
  }
  mask = allocate_buffer(4);
  mask[0] = random(256); mask[1] = random(256); mask[2] = random(256); mask[3] = random(256);
  sz = sizeof(b);
  if(sz<126){
    to_send = allocate_buffer(sz + 6);
    to_send[0] = 128 | opcode; // FIN and opcode
    to_send[1] = 128 | sz; // mask bit and 7-bit size
    to_send[2..5] = mask; // 4 byte mask
    payload_start = 6;
  }
  else if(sz<0x10000){
    to_send = allocate_buffer(sz + 8);
    to_send[0] = 128 | opcode;
    to_send[1] = 128 | 126; // mask bit and 7-bit special value saying size is 2 bytes or 16-bit
    to_send[2] = sz / 256; // higher bits of actual size
    to_send[3] = sz % 256; // lower bits of actual size
    to_send[4..7] = mask;
    payload_start = 8;
  }
  else {
    to_send = allocate_buffer(sz + 14);
    to_send[0] = 128 | opcode;
    to_send[1] = 128 | 127; // mask bit and 7-bit special value saying size is 8 bytes or 64-bit
    to_send[2] = (sz/0x100000000000000) % 256;
    to_send[3] = (sz/0x1000000000000) % 256;
    to_send[4] = (sz/0x10000000000) % 256;
    to_send[5] = (sz/0x100000000) % 256;
    to_send[6] = (sz/0x1000000) % 256;
    to_send[7] = (sz/0x10000) % 256;
    to_send[8] = (sz/0x100) % 256;
    to_send[9] = sz % 256;
    to_send[10..13] = mask;
    payload_start = 14;
  }
  for(int i=0;i<sz;i++){ // encode payload with mask
    to_send[i+payload_start] = b[i] ^ mask[i%4];
  }
  debug("  about to do the socket_write");
  socket_write(socket, to_send);
 }
void send(mixed arg){
  buffer b; int opcode, t;
  debug("send called, typeof="+typeof(arg));
  if(previous_object() != callback_object){ debug(sprintf("halted send, was called by %O",previous_object())); return; }
  if(!connected){
    debug("  not connected though, stopping here");
    return;
  }  
  if(bufferp(arg)) { b = arg; opcode=OPCODE_BINARY; }
  else if(stringp(arg)) {  b=allocate_buffer(sizeof(arg));  write_buffer(b,0,arg); opcode=OPCODE_TEXT; }
  else { debug("send given "+typeof(arg)+" instead of string or buffer"); return; }
  if((usetype & 12) == 4){ opcode = OPCODE_TEXT; }
  if((usetype & 12) == 8){ opcode = OPCODE_BINARY; }
  debug("  sending "+sizeof(b)+" bytes with opcode "+opcode);
  send_frame(b,opcode);
 }
void drop_connection(string str){
   debug("drop_connection got called, str="+str);
   if((origin() != "local") && (previous_object()!=callback_object)) { debug("halt drop_connection, origin="+origin()+", PO:"+identify(previous_object())); return; }
   send_frame(allocate_buffer(0),OPCODE_CLOSE);
   socket_close(socket);
   connected = 0; ws_server=0;
   if(callback_object) call_other(callback_object,close_cb,(str?str:"unknown reason"));
 }
void set_datatype(int i){
  usetype = i;
 }
void set_subprotocol(string str){
  subprotocol = str;
 }
int clean_up(){
  if(callback_object) return 1; // if the object that set this up is still loaded then this should stay loaded too
  remove();
  return 1;
 }
void remove(){
  // Debating on whether should block someone from desting it if they are not the one who loaded it and not admin
  debug("removing");
  if(connected) drop_connection("WS client object had remove() called on it");
  destruct(this_object());
  return;
 }
