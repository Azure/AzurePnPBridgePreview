gcc -g static-test.c curl_wrapper.c ../helpers/string_manipulation.c -lcurl -o static-test-x64 -lpthread
# gcc -g stream-test.c curl_wrapper.c ../helpers/string_man -lcurl -o stream-test-x64 -lpthread