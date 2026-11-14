#!/bin/bash

HAVOC_BIN="$1"
TEST_BODY="/tmp/body"

trap "pkill havoc-debug; rm $TEST_BODY" EXIT
trap 'exit 130' INT TERM

"$HAVOC_BIN" -d tests/fixture &

sleep 0.5

code=$(curl -s -o $TEST_BODY --path-as-is -X GET -w "%{response_code}" http://localhost:8080/)
if [ "$code" != 200 ]; then
  echo "CASE 1 (GET /): Expected response code 200 got $code"
  exit 1
fi
if ! cmp -s $TEST_BODY tests/fixture/index.html; then
  echo "CASE 1 (GET /): Expected body of file index.html"
  exit 1
fi
echo "CASE 1 (GET /): Success"

code=$(curl -s -o /dev/null --path-as-is -X GET -w "%{response_code}" http://localhost:8080/none)
if [ "$code" != 404 ]; then
  echo "CASE 2 (404): Expected response code 404 got $code"
  exit 1
fi
echo "CASE 2 (404): Success"

data=$(curl -s -o /dev/null --path-as-is -X HEAD -w "%{response_code} %{size_download}" http://localhost:8080/)
if [ "$data" != "200 0" ]; then
  echo "CASE 3 (HEAD /): Expected response code 200 with 0 download size got $data"
  exit 1
fi
echo "CASE 3 (HEAD /): Success"

code=$(curl -s -o /dev/null --path-as-is -X GET -w "%{response_code}" http://localhost:8080/../smoke.sh)
if [ "$code" != 404 ]; then
  echo "CASE 4 (/../smoke.sh): Expected response code 404 got $code"
  exit 1
fi
echo "CASE 4 (/../smoke.sh): Success"

path=$(printf "A%.0s" {1..1024})
code=$(curl -s -o /dev/null --path-as-is -X GET -w "%{response_code}" http://localhost:8080/$path)
if [ "$code" != 414 ]; then
  echo "CASE 5 (/<long-uri>): Expected response code 414 got $code"
  exit 1
fi
echo "CASE 5 (/<long-uri>): Success"

code=$(printf 'GET / HTTP/1.1\r\nHost: x\r\nBadHeader\r\n\r\n' | nc -q1 localhost 8080 | head -1 - $raw | cut -d ' ' -f2-2)
if [ "$code" != 400 ]; then
  echo "CASE 6 (invalid header): Expected response code 400 got $code"
  exit 1
fi
echo "CASE 6 (invalid header): Success"

exit 0
