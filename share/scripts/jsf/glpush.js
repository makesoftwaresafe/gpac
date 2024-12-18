/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2020-2024
 *					All rights reserved
 *
 *  This file is part of GPAC / AVGenerator filter
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

import {WebGLContext} from 'webgl'
import {Texture, Matrix} from 'evg'
import { Sys as sys } from 'gpaccore'


//metadata
filter.set_name("glpush");
filter.set_class_hint(GF_FS_CLASS_AV);
filter.set_desc("GPU texture uploader");
filter.set_version("1.0");
filter.set_author("GPAC team");
filter.set_help("This filter pushes input video streams to GPU as OpenGL textures. It can be used to simulate hardware decoders dispatching OpenGL textures");

//raw video in and out
filter.set_cap({id: "StreamType", value: "Video", inout: true} );
filter.set_cap({id: "CodecID", value: "raw", inout: true} );

let gl=null;
filter.initialize = function() {
  let gpac_help = sys.get_opt("temp", "gpac-help");
  let gpac_doc = (sys.get_opt("temp", "gendoc") == "yes") ? true : false;
  //don't initialize gl if doc gen or help
  if (gpac_help || gpac_doc) return;
}

let pids=[];

function cleanup_texture(pid)
{
  pid.o_textures.forEach( t => {
    if (t.id) gl.deleteTexture(t.id);

  });
  pid.o_textures = [];  
}
filter.configure_pid = function(pid)
{
  //init gl only when configuring input - doing it at init may result in uninitialized GL
  if (!gl) {
    gl = new WebGLContext(16, 16);
    if (!gl) return GF_SERVICE_ERROR;
  }

  if (typeof pid.o_pid == 'undefined') {
      pid.o_pid = this.new_pid();
      pid.o_pid.i_pid = pid;
      pid.o_pid.wtf = "toto";
      pid.o_w = 0;
      pid.o_h = 0;
      pid.o_pf = '';
      pid.o_textures = [];
      pid.frame_pending = false;
      pid.cache_pck = null;
      pids.push(pid);
  }
  pid.o_pid.copy_props(pid);
  pid.o_pid.set_prop('Stride', null);
  pid.o_pid.set_prop('StrideUV', null);
  let i_w = pid.get_prop('Width');
  let i_h = pid.get_prop('Height');
  let i_pf = pid.get_prop('PixelFormat');
  let i_stride = pid.get_prop('Stride');
  let i_stride_uv = pid.get_prop('StrideUV');
  //not ready yet
  if (!i_w || !i_h || !i_pf) return;
  //same config
  if ((i_w == pid.o_w) && (i_h == pid.o_h) && (i_pf == pid.o_pf)) return;

  pid.o_w = i_w;
  pid.o_h = i_h;
  pid.o_pf = i_pf;
  cleanup_texture(pid);
  print(GF_LOG_DEBUG, 'Configure pid ' + pid.o_w + 'x' + pid.o_h + '@' + pid.o_pf);

    if (!i_stride) i_stride = i_w;

  if ((i_pf == 'nv12') || (i_pf == 'nv21')) {
    let tx = {};
    tx.id=0;
    tx.offset=0;
    tx.size=i_stride*i_h;
    tx.w=i_w;
    tx.h=i_h;
    tx.fmt = gl.LUMINANCE;
    pid.o_textures.push(tx);

    tx = {};
    tx.id=0;
    tx.offset=i_stride*i_h;
    tx.size=i_stride*i_h/2;
    tx.w=i_w/2;
    tx.h=i_h/2;
    tx.fmt = gl.LUMINANCE_ALPHA;
    pid.o_textures.push(tx);
  }
  else if ((i_pf == 'yuv420') || (i_pf == 'yvu420')) {
    if (!i_stride_uv) i_stride_uv = i_stride/2;
    let tx = {};
    tx.id=0;
    tx.offset=0;
    tx.size=i_stride*i_h;
    tx.w=i_w;
    tx.h=i_h;
    tx.fmt = gl.LUMINANCE;
    pid.o_textures.push(tx);

    tx = {};
    tx.offset=i_stride*i_h;
    tx.size=i_stride_uv*i_h/2;
    tx.id=0;
    tx.w=i_w/2;
    tx.h=i_h/2;
    tx.fmt = gl.LUMINANCE;
    pid.o_textures.push(tx);

    tx = {};
    tx.id=0;
    tx.offset=i_stride*i_h + i_stride_uv*i_h/2;
    tx.size=i_stride_uv*i_h/2;
    tx.w=i_w/2;
    tx.h=i_h/2;
    tx.fmt = gl.LUMINANCE;
    pid.o_textures.push(tx);
  }
  else if ((i_pf == 'rgba') || (i_pf == 'argb') || (i_pf == 'abgr') || (i_pf == 'bgra')
      || (i_pf == 'xrgb') || (i_pf == 'rgbx') || (i_pf == 'xbgr') || (i_pf == 'bgrx')
  ) {
    let tx = {};
    tx.id=0;
    tx.offset=0;
    tx.size=i_stride*i_h*4;
    tx.w=i_w;
    tx.h=i_h;
    tx.fmt = gl.RGBA;
    pid.o_textures.push(tx);
  }
  else if ((i_pf == 'rgb') || (i_pf == 'bgr')) {
    let tx = {};
    tx.id=0;
    tx.offset=0;
    tx.size=i_stride*i_h*3;
    tx.w=i_w;
    tx.h=i_h;
    tx.fmt = gl.RGB;
    pid.o_textures.push(tx);
  }
  else if ((i_pf == 'algr') || (i_pf == 'gral')) {
    let tx = {};
    tx.id=0;
    tx.offset=0;
    tx.size=i_stride*i_h*2;
    tx.w=i_w;
    tx.h=i_h;
    tx.fmt = gl.LUMINANCE_ALPHA;
    pid.o_textures.push(tx);
  }
  else if ((i_pf == 'grey')) {
    let tx = {};
    tx.id=0;
    tx.offset=0;
    tx.size=i_stride*i_h;
    tx.w=i_w;
    tx.h=i_h;
    tx.fmt = gl.LUMINANCE;
    pid.o_textures.push(tx);
  }
  else {
    //todo
    print(GF_LOG_ERROR, "Unsupported pixel format " + i_pf + " for GPU upload");
    return GF_NOT_SUPPORTED;
  }
}

filter.remove_pid = function(pid)
{
  cleanup_texture(pid);
  pids.splice(pids.indexOf(pid), 1);
}


filter.process = function()
{

  pids.forEach(pid => {
    if (pid.frame_pending) return;
    let ipck = pid.get_packet();
    if (!ipck) {
      if (pid.eos) pid.o_pid.eos = true;
      return;
    }
    //frame is already in gpu, simply forward
    if (ipck.frame_ifce_gl) {
      pid.o_pid.forward(ipck);
      pid.drop_packet();
      return;
    }

    //frame is an interface, force fetching data (no per-plane access in JS)
    let clone_pck = null;
    if (ipck.frame_ifce) {
      clone_pck = ipck.clone(pid.cache_pck);
      ipck = clone_pck;
    }

    //upload all textures
    pid.o_textures.forEach( (tx, index) => {
      let ab = new Uint8Array(ipck.data, tx.offset, tx.size);
      if (!tx.id) tx.id = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, tx.id);
      gl.texImage2D(gl.TEXTURE_2D, 0, tx.fmt, tx.w, tx.h, 0, tx.fmt, gl.UNSIGNED_BYTE, ab);
    }); 

    //send new frame interface in blocking mode since the same set of textures is used for each packet
    let opck = pid.o_pid.new_packet( 
        (pid, pck, plane_idx) => {
          let i_pid = pid.i_pid;
          if (plane_idx >= i_pid.o_textures.length) return null;
          return {"id": i_pid.o_textures[plane_idx].id, 'fmt': gl.TEXTURE_2D};
        },
        (pid, pck) => {
            let i_pid = pid.i_pid;
            i_pid.frame_pending = false;
        },
        true
    );
    opck.copy_props(ipck);
    pid.frame_pending = true;
    opck.send();
    pid.drop_packet();
    if (clone_pck)
        clone_pck.discard();
  }); 

  return GF_OK;
}

