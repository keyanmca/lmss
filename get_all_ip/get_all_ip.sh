#!/bin/bash
cat /dev/null > /usr/local/nginx/conf/ip.conf
ipAll=`ifconfig -a | grep "inet addr" | awk '{print $2}' | awk -F ":" '{print $2}'`
for ip in $ipAll
do
	echo $ip
	curl -s  http://whois.pconline.com.cn/ip.jsp?ip=$ip -o url
	iconv -f GB2312 -t UTF-8 url > url.new;
	cat url.new | grep 局域网;
	if [ $? = 0 ]
	then
	   localIp=$ip
	   echo "juyuwang"
	fi
	cat url.new | grep 电信;
	if [ $? == 0 ]
	then
	   DXIp=$ip
	   echo "dianxin"
	fi
	cat url.new | grep 联通;
	if [ $? == 0 ]
	then
	   LTIp=$ip
	   echo "liantong"
	fi
	sleep 1
done

echo "localip: $localIp"
echo "DX: $DXIp"
echo "LT: $LTIp"

VS_MIp="115.231.96.195"
VS_SIp="115.231.96.196"
echo "$DXIp#1">> /usr/local/nginx/conf/ip.conf
echo "$LTIp#2" >> /usr/local/nginx/conf/ip.conf
echo "$VS_MIp#3" >> /usr/local/nginx/conf/ip.conf
echo "$VS_SIp#3" >> /usr/local/nginx/conf/ip.conf

echo $VSIp

if [ $1 ]; then
	redis-cli -h 10.4.22.249 -p 6379 set $localIp'#1' $DXIp:$1
	redis-cli -h 10.4.22.249 -p 6379 set $localIp'#2' $LTIp:$1
	redis-cli -h 10.4.22.249 -p 6379 set $localIp'#3' $VS_MIp:$1
	redis-cli -h 10.4.22.249 -p 6379 set $localIp'#3' $VS_SIp:$1
else 
	redis-cli -h 10.4.22.249 -p 6379 set $localIp'#1' $DXIp:80
	redis-cli -h 10.4.22.249 -p 6379 set $localIp'#2' $LTIp:80
	redis-cli -h 10.4.22.249 -p 6379 set $localIp'#3' $VS_MIp:80
	redis-cli -h 10.4.22.249 -p 6379 set $localIp'#3' $VS_SIp:80
fi

