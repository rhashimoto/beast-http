import os
import sys

# Define build variables.
vars = Variables()
vars.Add(EnumVariable('target', 'Build type', 'debug', allowed_values=('debug', 'release')))

# Create the base environment.
env = Environment(ENV=os.environ, variables=vars)
env.Decider('MD5-timestamp')

# Target configuration.
if env['target'] == 'debug':
    env.Append(CXXFLAGS=['-g'])

elif env['target'] == 'release':
    env.Append(CXXFLAGS=['-O2'])
    env.Append(CPPDEFINES=['NDEBUG', 'BOOST_DISABLE_ASSERTS'])

# Project configuration.
env.Append(CPPDEFINES=['BOOST_LOG_DYN_LINK'])
env.Append(CXXFLAGS=['-std=c++14', '-pthread'])
env.Append(CXXFLAGS=['-Wall', '-Werror', '-Wno-unknown-pragmas'])
env.Append(CXXFLAGS=['-Wno-psabi'])

env.Append(LINKFLAGS=['-pthread'])
env.Append(LIBS=['boost_log', 'boost_system', 'boost_thread'])

test = env.Program('test', 'test.cpp')
env.Default(test)
