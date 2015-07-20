local json = require 'json.json'

local jsonobj = {
    key = {
		cluster_id = 0,
		nginx_id  = 0
    },
    value = {
		port = 80,
		h_port = 8080,
		internal_ip = '10.4.22.118',
		telecom_ip = '115.231.96.59',
		unicom_ip = '101.71.27.59'
	}
}

local jsonstr = json.encode(jsonobj)

local url = 'http://10.4.22.249:80/configure'

local http = require 'socket.http'
local body, code, header = http.request{
    url = url,
    headers = { jsonstr = jsonstr }
}
