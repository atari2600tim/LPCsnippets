/*
Client for Grapevine intermud system
After I tested out my websocket object, I added basic Grapevine code into it,
and then later I separated this out again on Feb 26 2019.
I haven't done anything to make it easy to integrate into your game, it barely works in my stock TMI-2.

10.240.0.4 4001 is my own computer running a copy of the grapevine server on an unencrypted connection.
To connect to the official server, you will want to set up stunnel on your local system.
Tell stunnel to listen on some port and have it forward you to the https port on grapevine.haus.

Your /etc/stunnel/stunnel.conf file should have something like the next 4 lines:
[grapevine]
client = yes
accept = 2000
connect = grapevine.haus:443

Also set up your firewall to block outsiders from reaching that port.

Then change the setup_websocket line below to "127.0.0.1 2000" or whatever port number you picked.

Actually instead of rambling here, just search for CHANGEME and those are the first
things you almost certainly need to change for your game.
If you're not using a stock TMI-2 mudlib on FluffOS then there's probably more than that.

Also you'll want a command to interact with this.  Mine consists of this single line:
int cmd_grapevine(string str){ return "/adm/daemons/network/grapevine.c"->command(str); }

Also you should have a simul_efun for generate_uuid() like the one at https://github.com/atari2600tim/timmud/blob/master/uuid.c
And here is something for parsing json: https://github.com/atari2600tim/timmud/blob/master/json.c
-Tim

NOTES/TODO/etc:
If it disconnects, then reload this object.
Currently it saves no settings or chat history, but does log to some files in /log/adm/ for debugging.
It does not try to reconnect, will have to reset certain variables before doing that.
It is completely self-contained, does not send channel messages to channel daemon,
does not allow individuals to tune in/out channels or announcements.
Same in other direction... the login process does not tell this that you've logged in,
this just checks every time it has a heartbeat and then announces if it sees a different list than last time.
I haven't implemented tells and stuff.
There is zero security so far, any newbie wiz can manually call all the functions in here, chatting in your name and worse.
*/

// "DevGame" / "Development Game" is in the list automatically if you install a local copy of Grapevine server, they start with 62a and 3ab.
#ifndef CLIENT_ID
//if you're not going to a personal development copy of the server then CHANGEME to whatever the web site tells you to use
// I used ifndef so that you can leave this here and put yours at the very top separately if you like
#define CLIENT_ID "62a8988e-f505-4e9a-ad21-e04e89f1b32b"
#define CLIENT_SECRET "3ab47e7e-010f-488a-b7d6-a474440efda5" 
#endif

object ws;

int authenticated; // 0 or 1 if we got the authentication success message
mapping game_list; // key is game name, value is payload from games/status message
mapping players_list; // key is game name, value is array of players at remote games
string *channel_list; // it doesn't seem to tell you about channels being created or deleted, might have to manually track this
string *games_online_list;
mapping ref_channel_subscribe; // list of subscribe requests that we haven't gotten a response from
mapping ref_channel_unsubscribe; // list of subscribe requests that we haven't gotten a response from
mapping ref_channel_send; // list of outgoing messages that the server should acknowledge
mapping restart_packet; // payload from last restart packet we got, so that when connection drops we can look at this and know how long to wait before retry
mapping ref_game_status;
string *declared_players; // players we told the network about

void subscribe_channel(string str);

int is_grapevine_user(object ob){
    // return 1 if this person uses the grapevine thing
    if(!ob) return 0;
    if(ob->query_env("grapevine")) return 1;
    return 0;
 }

void setup(){
  string err;
  ws = new("/obj/net/wsclient.c"); // CHANGEME
  err = ws->setup_websocket("10.240.0.4 4001", "grapevine.haus", "/socket", "got_disconnected", "got_text", "got_connected"); // CHANGEME
}


void create(){
  authenticated = 0;
  game_list = ([ ]);
  players_list = ([]);
  channel_list = ({});
  games_online_list = ({});
  ref_channel_subscribe = ([]);
  ref_channel_unsubscribe = ([]);
  ref_channel_send = ([]);
  declared_players = ({});
  call_out("setup",3);
}

// 'set grapevine' to participate and be visible on the network, you see people log in/out, channels, muds online/offline, but not told about internals
// 'set debugWS' to see websocket debugging messages, which means 13 lines of messages every heartbeat (15 seconds) at minimum
// 'set debugGOS' to see Gossip/Grapevine debugging messages, which means 6 lines per heartbeat
// 'set debugHigh' to see high-level messages, same messages as 'set grapevine' except without having to be listed online and all
void debug(string str){
  foreach(object ob in users()){
    if(adminp(ob) && ob->query_env("debugWS")) tell_object(ob, "GOS(WSpart): "+str+"\n");
  }
  log_file("adm/gos.ws",""+time()+": "+str+"\n");
 }

void deb(string str){
  foreach(object ob in users()){
    if(adminp(ob) && ob->query_env("debugGOS")) tell_object(ob, "GOSclient: "+str+"\n");
  }
  log_file("adm/gos",""+time()+": "+str+"\n");
 }
void debHigh(string str){
  foreach(object ob in users()){
    if(ob->query_env("grapevine") || ob->query_env("debugHigh")) tell_object(ob, "%^MAGENTA%^Grape%^GREEN%^vine%^RESET%^: "+str+"\n");
  }
  log_file("/adm/gos.high",""+time()+": "+str+"\n");
}


void send_json(mixed m){
  string str;
  deb("send_json called");

  str = "/u/t/tim/json/lpc-json/tim-json.c"->json_encode(m); // CHANGEME tim-json has a cheap hack I did so 0 will work
  ws->send(str);
}

void got_connected(){
    debHigh("Got connected, about to authenticate");
   deb("got_connected was called, going to authenticate");
  declared_players = ({ });
  send_json( 
      ([ "event":"authenticate",
         "payload":([
           "client_id": CLIENT_ID,
           "client_secret": CLIENT_SECRET,
           "supports": ({ "channels","games","players" }), // must contain at least channels, could have channels, players, tells, games, achievements
           "channels": ({ }),  
           /*
           If you subscribe to an invalid channel via the authenticate packet then you get this message:
           {"error":"Could not subscribe to 'has some spaces'","event":"channels/subscribe","status":"failure"}
           If you subscribe to an invalid channel via a subscribe packet, then you get this:
           {"error":"Could not subscribe to \"spaces here\"","event":"channels/subscribe","ref":"0cc2a7f1-70bd-41c3-b196-67850a4064a3","status":"failure"}
           I want to programmatically recognize the problematic channels, so best to just do it separately and get that ref.
           I don't expect to be booted from channels or anything, but seems simpler
           */
           "version" : "2.3.0",
           "user_agent": __VERSION__ // optional
         ])
      ])
  );
}

void got_disconnected(string str){
    debHigh("We were disconnected");
    deb("got disconnected");
    authenticated = 0;
    if(restart_packet){
        deb("  restart packet from earlier suggests downtime of: "+restart_packet["downtime"]);
        // This is where I'd set up a reconnect thing once I implement that.
        return;
    }
    else{
        deb("  no restart packet was sent");
        return;
    }
    // possibly do something different if they sent us a 'close' frame with a message in it... if they told us bad authentication then shouldn't hammer it until they ban our IP or something
}

void got_text(string str){
  mapping map;
  string ref,a,b,c,*arrA, *arrB, *arrC, err;
  mapping pay;
  deb("got_text called, size="+sizeof(str));
  deb(sprintf(" about to try json_decode on [%s]...",str));
  err = catch{
   map = "/u/t/tim/json/lpc-json/tim-json.c"->json_decode(str); // CHANGME
  };
  if(err) deb(sprintf("Got error of [%s] decoding [%s]",err,str));
  deb(" ...did the json_decode");
  ref = map["ref"];
  if(ref) ref = lower_case(ref);
  pay = map["payload"];
  deb(sprintf("   got this: [%O]",save_variable(map)));

  switch(map["event"]){
    case "authenticate":
      deb("   about to handle authentication response");
      if(map["status"]=="success"){
        deb("     authentication success, version="+map["version"]);
        authenticated = 1;
        deb("     will send game and player status requests and announce players");
        debHigh("We have successfully authenticated");
        send_json( ([ "event":"games/status", "ref":generate_uuid() ]) );
        send_json( ([ "event":"players/status", "ref":generate_uuid() ]) );
// move this to a configuration setting
subscribe_channel("gossip");
subscribe_channel("announcements");
subscribe_channel("testing");
        // Should I send a sign-in packet upon login regarding each existing player?
        // For the server, "each beat fully replaces the list, ensuring it keeps in sync".
        // players/sign-in "is for between the beats and notifying connected games".
        // Beats are pretty quick though, like 5 seconds.  I don't care about between beats so much, but I do care about notifying the other games, if heartbeat doesn't do it.
        // Try starting it up, connect NodeJS (which has one player), and then connect one.c, and see if one.c is notified about the player on NodeJS.
        // ...I set up two.c and three.c, and when I mark myself as online, they both start sending heartbeats that include me, but it does nothing to notify one.c that people came online,
        // so I do indeed need to tell them
        declared_players = uniq_array((filter_array(users(),(: visible($1) && is_grapevine_user($1) :)))->query("cap_name"));
        foreach(a in declared_players){
            send_json( ([ "event":"players/sign-in","ref":generate_uuid(),"payload":(["name":a]) ]) );
        }
      }
      else{
        deb("     authentication something other than success, dropping connection ["+save_variable(map)+"]");
        ws->drop_connection();
      }
      return;
    case "heartbeat":
      deb("   about to handle heartbeat, it requires a response");

      // compare players with last heartbeat and then announce a sign-in or out, using declared_players array
      // a = players, b=new, c=old
      arrA = uniq_array((filter_array(users(),(: visible($1) && is_grapevine_user($1) :)))->query("cap_name"));
      arrB = arrA - declared_players;
      arrC = declared_players - arrA;
      foreach(a in arrB){
        send_json( ([ "event":"players/sign-in","ref":generate_uuid(),"payload":(["name":a]) ]) );
        declared_players += ({ a });
      }
      foreach(a in arrC){
        send_json( ([ "event":"players/sign-out","ref":generate_uuid(),"payload":(["name":a]) ]) );
        //declared_players -= ({ a });
      }

      send_json( ([ 
          "event":"heartbeat",
          "payload":([
            "players": uniq_array((filter_array(users(),(: visible($1) && is_grapevine_user($1) :)))->query("cap_name"))
            // players is optional but required to get tells and whatnot
          ])
      ]) );
      return;
    case "restart":
      deb("   about to handle restart");
      deb("     given downtime is:"+map["payload"]["downtime"] );
      debHigh("Notification that server will be down for "+map["payload"]["downtime"]+" seconds.\n");
      restart_packet = map["payload"];
      return;
    // for actually integrating channels I should probably add an external object to interact with
    // the channel daemon like how /adm/daemons/network/I3/channel.c does and how /adm/etc/channels does
    case "channels/subscribe": // response to subscribing to a channel
      deb("   channels/subscribe came in");
      a = ref_channel_subscribe[ref]; // a = channel name
      if(!a){
          debHigh("They acknowledge a channel subscribe that I don't remember requesting, uuid: "+ref+"\n");
        deb("    I never asked to subscribe to a channel using that ref though");
        return;
      }
      if(map["status"]=="failure"){
          debHigh("They reject a channel subscribe: "+a);
        deb("      failed to subscribe to channel ["+a+"]");
        deb("         error given:"+map["error"]);
        map_delete(ref_channel_subscribe,ref);
      }
      else{
          debHigh("They accept a channel subscribe to: "+a);
        deb("       success in subscribing to ["+a+"]");
        channel_list += ({ a });
        map_delete(ref_channel_subscribe,ref);
      }
      return;
    case "channels/unsubscribe":
      deb("   channels/unsubscribe came in");
      a = ref_channel_unsubscribe[ref];
      if(!a){
          debHigh("They respond to a channel unsubscribe that I don't remember requesting, uuid: "+ref);
        deb("    I never asked to unsubscribe to a channel using that ref though");
        return;
      }
      channel_list -= ({ a });
      debHigh("They accept a channel unsubscribe to: "+a);
      map_delete(ref_channel_unsubscribe,ref);
      deb("    unsubscribed to ["+a+"]");
      return;
    case "channels/broadcast":
      deb("   got a channel broadcast");
      deb(sprintf("CHANMSG(fromOthers)-<%s>%s@%s: %s",pay["channel"],pay["name"],pay["game"],pay["message"]));
      debHigh(sprintf("CHANMSG(fromOthers)-<%s>%s@%s: %s",pay["channel"],pay["name"],pay["game"],pay["message"]));
      log_file("adm/gos.chan",sprintf("CHANMSG(fromOthers)-%s-<%s>%s@%s: %s\n",ctime(time()),pay["channel"],pay["name"],pay["game"],pay["message"]));
      return;
    case "channels/send":
      deb("  got a channels/send message, confirming they got our message");
      pay=ref_channel_send[ref]; // content of message we sent
      if(!pay){
          debHigh("They acknowledge a channel message I don't remember sending, uuid: "+ref);
          deb("   I don't recognize that ref on the chan/send acknowledgement");
          return;
      }
      deb(sprintf("CHANMSG(fromUsAcknowledged)-<%s>%s: %s",pay["channel"],pay["name"],pay["message"]));
      debHigh(sprintf("CHANMSG(fromUsAcknowledged)-<%s>%s: %s",pay["channel"],pay["name"],pay["message"]));
      debHigh(sprintf("CHANMSG(fromUsAcknowledged)-%s-<%s>%s: %s",ctime(time()),pay["channel"],pay["name"],pay["message"]));
      log_file("adm/gos.chan",sprintf("CHANMSG(fromUSAcknowledged)-%s-<%s>%s: %s\n",ctime(time()),pay["channel"],pay["name"],pay["message"]));
      map_delete(ref_channel_send,ref);
      // should I delay printing our channel message until it is confirmed to have gone out?
      //  does channel daemon properly handle i3 filtered channels which is same basic concept?
      return;
    case "players/sign-in":
      if(!map["payload"]) return; // server is acknowledging that I said somebody is online, I'll ignore it; only care about announcements from others
      a = pay["game"];
      b = pay["name"];
      deb("  players/signin regarding game ["+a+"] and player ["+b+"] ");
        debHigh(sprintf("%s@%s logged in",b,a));
      if(!players_list[a]){
         players_list[a] = ({ });
      }
      players_list[a] += ({ b });
      return;
    case "players/sign-out":
      if(!map["payload"]) return; // confirming one I sent out?
      a = pay["game"];
      b = pay["name"];
      deb("  players/signout regarding game ["+a+"] and player ["+b+"] ");
      debHigh(sprintf("%s@%s logged out",b,a));
      if(!players_list[a]){
         players_list[a] = ({ });
      }
      players_list[a] -= ({ b });
      return;
    case "players/status":
      a = pay["game"];
      deb("  players/status regarding game ["+a+"] and players ["+identify(pay["players"])+"] ");
      debHigh("Players update from "+a+", players are "+implode(pay["players"],", "));
      if(!players_list[a]){
         players_list[a] = ({ });
      }
      players_list[a] = pay["players"];
      return;
    case "tells/send":
    case "tells/receive":
    break;
    case "games/connect":
      debHigh(pay["game"]+" is online");
      deb("  games/connect tells us that "+pay["game"]+" is online");
      games_online_list += ({ pay["game"] });
      send_json( ([ "event":"games/status", "ref":generate_uuid(), "payload":([ "game":pay["game"] ]) ]) );
      deb("   asked the server about them");
      return;
    case "games/disconnect":
      debHigh(pay["game"]+" is offline");
      deb("  games/disconnect tells us that "+pay["game"]+" is offline");
      games_online_list -= ({ pay["game"] });
      return;
    case "games/status":
      deb("   games/status came in with ["+save_variable(map)+"]");
      if(map["status"]=="failure"){
          deb("  games/status sent us failure message");
          deb("     ERROR WAS: "+map["error"]);
          debHigh("Game status message uuid "+map["ref"]+" for (gamePlaceholder) gave us error: "+map["error"]);
          return;
      }
      if(!pay){
          debHigh("Game status packet that confuses me");
          deb("  no payload for games/status, but it did not give us status=failure, I think it shouldn't reach this");
          return;
      }
      deb(sprintf("  I think the games/status thing is okay, map=%O, pay=%O",map, pay));
      deb("    gonna try(788)....");
      games_online_list += ({ pay["game"] });
      game_list[pay["game"]] = pay;
      debHigh(pay["game"]+" updated game status");
      deb("    ...did it(788)");
      return;
    case "achievements/sync":
    case "achievements/create":
    case "achievements/update":
    case "achievements/delete":
      break;
    default:
      deb("   event is UNLISTED in specs: "+map["event"]);
      log_file("adm/gos.unknown",sprintf("unknown event name: %O\n\n",map));
      break;
  }
  debHigh("Unhandled packet of type "+map["event"]+"\n");
  deb("   event went unhandled: "+map["event"]);
  log_file("adm/gos.unhandled",sprintf("unhandled event: %O\n\n",map));
  deb("...reached the end of got_text");
}

void got_binary(buffer buf){
  string str;
  deb("got_binary called unexpectedly... size="+sizeof(buf));
  str = read_buffer(buf);
  got_text(str);
}


string get_info(){
    return sprintf("authenticated: %O\nkeys of game_list: %O\ngames_online_list: %O\nplayers_list: %O\nchannel_list: %O\nref_channel_subscribe: %O\nref_channel_unsubscribe: %O\nrestart_packet: %O\n",
      authenticated, keys(game_list), games_online_list, players_list, channel_list, ref_channel_subscribe, ref_channel_unsubscribe, restart_packet);
}

string get_game_list(){
    return dump_variable(game_list);
}

void subscribe_channel(string str){
    string u;
    deb("called subscribe_channel, given ["+str+"]");
    u = generate_uuid();
    ref_channel_subscribe[u] = str;
    send_json( ([ "event":"channels/subscribe", "ref":u, "payload":(["channel":str]) ]) );
    return;
}
void unsubscribe_channel(string str){
    string u;
    deb("called unsubscribe_channel, given ["+str+"]");
    u = generate_uuid();
    ref_channel_unsubscribe[u] = str;
    send_json( ([ "event":"channels/unsubscribe", "ref":u, "payload":(["channel":str]) ]) );
    return;
}

void send_chan_msg(string chan, string msg){
    string u;
    mapping pay;
    deb(sprintf("send_chan_msg called, with chan=%s and msg=%s",chan,msg));
    if(member_array(chan, channel_list)==-1){
        deb(" not subscribed to that channel so stopping");
        return;
    }
    u = generate_uuid();
    pay = ([ "channel":chan, "name":this_player()->query("cap_name"), "message":msg ]);
    ref_channel_send[u] = pay;
    send_json( ([ "event":"channels/send", "ref":u, "payload": pay ]) );
    deb(" sent message");
}

string fix_mud_name(string str){
    // Given name in lowercase or whatever, it will match it up with another.
    string s,lc;
    lc = lower_case(str);
    foreach(s in keys(game_list)){
      if(lower_case(s)==lc) return s;
    }
    return 0;
}

int command(string str){
    string output, name, *names;
    mixed info;
    if((!str) || (str=="help")){
        write(@EndText
Arguments for the Grapevine thing:
  help or blank - this message
  list - list of games
  info [name] - info about a specific game
  gossip [message] - send out a message to the gossip channel
 debugging ones...
  allinfo - dump of the variables this uses for debugging
  channels - list of channels
  subscribe [chan] - makes the game subscribe to channel
  unsubscribe [chan] - makes the game unsubscribe to channel
  playerstatus - send a players/status packet to ask the server to tell us about online players
 note:
  Tells and achievements are not implemented yet.
  It automatically subscribes to gossip, testing, announcements upon login
  It automatically asks about players upon login, and should keep track of updates
  There isn't a way to personally tune in/out of channels and watch for specific events and such.
  For now, you can 'set grapevine' or 'unset grapevine', this determines if you see messages and if you are visible to network
  Later all this stuff will be integrated with the game instead of through this grapevine command
EndText);
        return 1;
        return notify_fail("Args: list, info [name], games1, games2, games3, games4, allinfo\n");
    }
    if(!authenticated){
        return notify_fail("It has not authenticated yet\n");
    }
    if(str=="list"){
        output=("Grapevine game list\n_______ShortName________DisplayName_______\n");
        names=keys(game_list);
        if(!sizeof(names)) return notify_fail("No games known yet\n");
        foreach(name in sort_array(keys(game_list),1)){
            info = game_list[name];
            output += sprintf("%-6s %-15s  %-30s\n",( (member_array(info["game"],games_online_list)==-1)? "[down]":""), info["game"],info["display_name"]);
        }
        this_player()->more(explode(output,"\n"));
        return 1;
    }
    else if(sscanf(str,"info %s",name)==1){
        info = fix_mud_name(name);
        if(info) name = info;
        if(!game_list[name]){
            printf("No game known called [%s], will request it though.\n",name);
            send_json( ([ "event":"games/status", "ref":generate_uuid(), "payload":([ "game":name ]) ]) );
            return 1;
        }
        info = game_list[info];
        output = "";
        output += sprintf("Game         : %s\n",info["game"]); //required
        output += sprintf("LongName     : %s\n",info["display_name"]); // required
        if(info["description"]) // doesn't say optional, but I got 0 on my sample thing
        output += sprintf("Description  : %s\n",implode(explode(wrap(info["description"],65),"\n"),"\n               "));
        if(info["homepage_url"])
        output += sprintf("Homepage     : %s\n",info["homepage_url"]); // required?
        if(info["supports"])
        output += sprintf("Supports     : %s\n",implode(game_list[name]["supports"],", "));
        if(info["players_online_count"])
        output += sprintf("Player count : %d\n",game_list[name]["players_online_count"]);
        if(info["user_agent"])
        output += sprintf("UserAgent    : %s\n",game_list[name]["user_agent"]);
        if(info["user_agent_repo_url"])
        output += sprintf("UserAgentURL : %s\n",game_list[name]["user_agent_repo_url"]);
        if(info["connections"]){
          output += "Connections:\n";
          foreach(mapping m in info["connections"]){
              if(m["type"]=="telnet")
                output += sprintf("        telnet : %s %d\n",m["host"],m["port"]);
              if(m["type"]=="secure telnet")
                output += sprintf(" secure telnet : %s %d\n",m["host"],m["port"]);
              if(m["type"]=="web")
                output += sprintf("    web client : %s\n",m["url"]);
          }
        }
        if(players_list[name])
        output += sprintf("Players on   : %s\n", implode(players_list[name],", "));
        write(output); return 1;
    }
    else if(sscanf(str,"gossip %s",name)==1){
      if(member_array("gossip",channel_list)==-1) return notify_fail("Not subscribed to gossip channel\n");
      write("Sending message out to network on gossip channel: "+name+"\n");
      send_chan_msg("gossip",name);
      return 1;
    }
    else if(sscanf(str,"testing %s",name)==1){
      if(member_array("testing",channel_list)==-1) return notify_fail("Not subscribed to testing channel\n");
      write("Sending message out to network on testing channel: "+name+"\n");
      send_chan_msg("testing",name);
      return 1;
    }
    else if(str=="channels"){
        output = "Channels we are subscribed to on grapevine network:\n";
        foreach(name in (channel_list)){
            output += sprintf(" %s\n", name);
        }
        write(output);
        return 1;
    }
    else if(sscanf(str,"subscribe %s",name)==1){
        if(member_array(name,channel_list)!=-1){
            return notify_fail(sprintf("Already subscribed to %s\n"));
        }
        if(member_array(name,values(ref_channel_subscribe))!=-1){
            return notify_fail(sprintf("Already requested to subscribe to %s (waiting for server to acknowledge)\n"));
        }
        subscribe_channel(name);
        write(sprintf("Sent a request to subscribe to %s.\n",name));
        return 1;
    }
    else if(sscanf(str,"unsubscribe %s",name)==1){
        if(member_array(name,channel_list)==-1){
            return notify_fail(sprintf("Already unsubscribed to %s\n"));
        }
        if(member_array(name,values(ref_channel_unsubscribe))!=-1){
            return notify_fail(sprintf("Already requested to unsubscribe to %s (waiting for server to acknowledge)\n"));
        }
        unsubscribe_channel(name);
        write(sprintf("Sent a request to unsubscribe to %s.\n",name));
        return 1;
    }
    else if(str=="playersstatus"){
                send_json( ([ "event":"players/status", "ref":generate_uuid() ]) );
                write("trying generic players/status msg");
                return 1;
    }
    else if(str=="test1"){
                send_json( ([ "event":"games/status", "ref":generate_uuid() ]) );
                write("trying generic games/status msg");
                return 1;
    }
    else if(str=="test2"){
                send_json( ([ "event":"games/status", "ref":generate_uuid(), "payload":([ "game":"Grapevine" ]) ]) );
                write("trying games/status msg for the ones listed in that seeds database thing");
                return 1;
    }
    else if(str=="test3"){
                send_json( ([ "event":"games/status", "ref":generate_uuid(), "payload":([ "game":"DevGame" ]) ]) );
                write("trying games/status msg for the ones listed in that seeds database thing");
                return 1;
    }
    else if(str=="test4"){
        return notify_fail("this disconnects\n");
                send_json( ([ "event":"games/status", "ref":generate_uuid(), "payload":([ "game":"Raisin" ]) ]) );
                write("trying games/status msg for the ones listed in that seeds database thing");
                return 1;
    }

    else if(str=="allinfo"){
      output = get_info();
      write(output);
      return 1;
    }
    else {
        return notify_fail("Not sure what was wanted there.\n");
    }
}

/*
 Should move the channel and tells and such into separate modules in another file that can be updated separately like I3 does
 */
