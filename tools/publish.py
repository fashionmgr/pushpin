#!/usr/bin/env python
#
# Copyright (C) 2015 Fanout, Inc.
#
# This file is part of Pushpin.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import sys
import json
import argparse
import tnetstring
import zmq


def ensure_utf8(i):
    if isinstance(i, dict):
        out = {}
        for k, v in i.iteritems():
            out[ensure_utf8(k)] = ensure_utf8(v)
        return out
    elif isinstance(i, list):
        out = []
        for v in i:
            out.append(ensure_utf8(v))
        return out
    elif isinstance(i, str):
        return i.encode("utf-8")
    else:
        return i


parser = argparse.ArgumentParser(description="Publish messages to Pushpin.")
parser.add_argument("channel", help="channel to send to")
parser.add_argument(
    "content", nargs="?", default="", help="content to use for HTTP body and WS message"
)
parser.add_argument("--code", type=int, help="HTTP response code to use. default 200")
parser.add_argument("-H", "--header", action="append", help="add HTTP response header")
parser.add_argument(
    "--spec",
    default="tcp://localhost:5560",
    help="zmq PUSH spec. default tcp://localhost:5560",
)
parser.add_argument("--close", action="store_true", help="close streaming requests")
parser.add_argument("--id", help="payload ID")
parser.add_argument("--prev-id", help="payload previous ID")
parser.add_argument("--sender", help="sender meta value")
parser.add_argument("--patch", action="store_true", help="content is JSON patch")
args = parser.parse_args()

headers = []
if args.header:
    for h in args.header:
        k, v = h.split(":", 1)
        headers.append([ensure_utf8(k), ensure_utf8(v.lstrip())])

meta = dict()
formats = dict()

if args.content:
    hr = {}
    if args.patch:
        hr[b"body-patch"] = ensure_utf8(json.loads(args.content))
    else:
        hr[b"body"] = ensure_utf8(args.content + "\n")
    if args.code is not None:
        hr[b"code"] = args.code
    if headers:
        hr[b"headers"] = headers
    formats[b"http-response"] = hr

if args.close:
    formats[b"http-stream"] = {b"action": b"close"}
elif args.content and not args.patch:
    formats[b"http-stream"] = {b"content": ensure_utf8(args.content + "\n")}

if args.content and not args.patch:
    formats[b"ws-message"] = {b"content": ensure_utf8(args.content)}

if not formats:
    print("error: nothing to send")
    sys.exit(1)

if args.sender:
    meta[b"sender"] = ensure_utf8(args.sender)

item = {b"channel": ensure_utf8(args.channel), b"formats": formats}

if args.id:
    item[b"id"] = ensure_utf8(args.id)
if args.prev_id:
    item[b"prev-id"] = ensure_utf8(args.prev_id)

if meta:
    item[b"meta"] = meta

ctx = zmq.Context()
sock = ctx.socket(zmq.PUSH)
sock.connect(args.spec)

sock.send(tnetstring.dumps(item))

print("Published")
