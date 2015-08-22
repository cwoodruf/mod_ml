#!/bin/sh
iptables -A INPUT -p udp --destination-port 11211 -s 206.12.16.0/24 -j ACCEPT
iptables -A INPUT -p tcp --destination-port 11211 -s 206.12.16.0/24 -j ACCEPT
iptables -A INPUT -p udp --destination-port 11211 -s 127.0.0.1 -j ACCEPT
iptables -A INPUT -p tcp --destination-port 11211 -s 127.0.0.1 -j ACCEPT
iptables -A INPUT -p udp --destination-port 11211 -j DROP
iptables -A INPUT -p tcp --destination-port 11211 -j DROP
iptables-save
