 echo 'hi there' | LD_PRELOAD=../libutp_preload.so openssl s_client -debug -msg -state -connect localhost:8080
