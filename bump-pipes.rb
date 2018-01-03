#!/usr/bin/ruby

if RUBY_PLATFORM =~ /linux/
  F_SETPIPE_SZ = 1031
  F_GETPIPE_SZ = 1032
  STDOUT.fcntl(F_SETPIPE_SZ, 1 << 20) rescue nil
  STDOUT.fcntl(F_SETPIPE_SZ, 16 << 20) rescue nil
  STDIN.fcntl(F_SETPIPE_SZ, 1 << 20) rescue nil
  STDIN.fcntl(F_SETPIPE_SZ, 16 << 20) rescue nil

  out_size = STDOUT.fcntl(F_GETPIPE_SZ) rescue nil
  in_size = STDIN.fcntl(F_GETPIPE_SZ) rescue nil

  STDERR.puts "stdin size: #{in_size}, stdout size: #{out_size}"
end

exec(*ARGV)
