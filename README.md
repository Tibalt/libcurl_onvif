# libcurl example for onvif and rtsp


## onvif
I spent quite some time to combine a linux timer fd with ```curl_multi_poll```. I tried with linux timer fd with select, libuv before quite smoothly. But this time, everything seems not right. At last I found out that the mistake lies in the misuse of ```time_create```  instead of ```timerfd_create```.  ```timerfd_create``` is the one could work with ```curl_multi_poll``` and other event loop. 


## rtsp
RTSP basically is not supported from libcurl. According to [Daniel](https://stackoverflow.com/a/59778142/14889996) Here I put a whole example including rtsp & interleaved RTP receive (the example get payload but no parse).

I put the code here to save other people's time. Good luck!
