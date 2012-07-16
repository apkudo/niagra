import Options
import sys

APPNAME = "niagrad"
VERSION = "1.0.5"
srcdir = "."
blddir = "build"

def set_options(opt):
  opt.tool_options("compiler_cc")

def configure(conf):
  conf.check_tool("compiler_cc")

def build(bld):
  obj = bld.new_task_gen("cc", "cprogram")
  obj.target = "niagrad"
  obj.source = """
     ./tools/niagrad/src/niagrad.c
     ./tools/niagrad/src/str.c
  """
  obj.includes = "./tools/niagrad/src/str.h"
  obj.install_path = "../bin"
