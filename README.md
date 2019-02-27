# timmud
These are unsorted things I made for using on TMI-2 mudlib using FluffOS.
Some day I might make them more generic to work on more setups.


base64.c - has simul_efuns for base64_encode and base64_decode

uuid.c - generate_uuid() randomly generates a UUID

json.c is slightly modified json functions from Lost Souls MUD (I probably broke it in some way beyond my understanding, but now strings like [0] are working)


wsclient - object that can connect to a websocket server and interact with it for you, requires base64 functions, and FluffOS packages for sockets (to access network) and crypto (to do sha1 hash)

ws_testmud.c - shows how to use wsclient.c to connect to a websocket game port

grapevine.c - clones and uses wsclient to use Grapevine chat network, requires everything wsclient requires plus json and uuid
