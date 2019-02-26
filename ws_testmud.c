/*
This connects to my own game via the websocket server, and shows me the login screen and sends "who" a few times after a while.
It is meant to show how the websocket client file works.
*/

object testmud;

void debug(string str){
  if(find_player("tim")) tell_object(find_player("tim"),str);
}

void setup(){
    string result;
    seteuid(getuid());
    testmud = new("/obj/net/wsclient.c");
    testmud->set_subprotocol("binary"); // native FluffOS server doesn't require this but websockify does
    testmud->set_datatype(1);
    result = testmud->setup_websocket("127.0.0.1 4335", "eternalfantasy.org", "/testmud", "close_callback", "read_callback", "online_callback");
    if(result){ debug(sprintf("error with setup: %s\n",result)); return; }
    debug(sprintf("setup gave no errors, %O is now working for %O\n", testmud, this_object()));
}

void create(){ // give a delay in case this file is loaded and then immediately unloaded
  call_out("setup",3);
}

void close_callback(string str){
  debug("close_callback: "+str+"\n");
}
void read_callback(string str){
  debug("RCB: "+str);
}
void online_callback(){
  debug("it is online now");
  call_out("delay",30);
  call_out("delay",60);
  call_out("delay",120);
}

void delay(){
    testmud->send("who\n");
}
