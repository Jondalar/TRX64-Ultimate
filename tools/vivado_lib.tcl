# SPDX-License-Identifier: GPL-3.0-or-later
# Helpers shared by the designs/*/build.tcl scripts.
#
# Two things are cached between builds, keyed by a hash of what goes into
# them (sources, settings, Vivado version):
#   - IP cores (the GTP wizard): generated and synthesized once.
#   - Out-of-context checkpoints of big, rarely changed blocks (the C64 core).
# The cache lives in $TRX_CACHE (tools/vivado.sh and CI mount a persistent
# directory there), else in .cache/ at the repository root.

# Where a build puts its reports: build.tcl sets TRX_BUILD's directory here.
if {![info exists ::trx_build]} { set ::trx_build build }

# Vivado threads: TRX_VIVADO_THREADS from the runner's config envs (builder: all of its
# cores), else 3 (the NAS has four and runs Gitea too; the Mac keeps to its limit).
if {[info exists ::env(TRX_VIVADO_THREADS)]} {
  set_param general.maxThreads $::env(TRX_VIVADO_THREADS)
} else {
  set_param general.maxThreads 3
}

proc trx_cache_dir {} {
    if {[info exists ::env(TRX_CACHE)] && $::env(TRX_CACHE) ne ""} {
        set d $::env(TRX_CACHE)
    } else {
        set d [file normalize [file join [file dirname [info script]] .. .cache]]
    }
    file mkdir $d
    return $d
}
# [info script] is only right while this file is being sourced.
set ::trx_cache [trx_cache_dir]

# sha256 over the given files (contents and names, in the given order) plus
# free text, cut to 16 hex digits.
proc trx_hash {files text} {
    set lines {}
    foreach f $files {
        lappend lines "[lindex [exec sha256sum $f] 0] [file tail $f]"
    }
    lappend lines $text [version -short]
    set tmp [file join $::trx_cache .hash.[pid]]
    set fh [open $tmp w]
    puts $fh [join $lines \n]
    close $fh
    set h [lindex [exec sha256sum $tmp] 0]
    file delete $tmp
    return [string range $h 0 15]
}

# Create, generate and synthesize an IP once per configuration; later builds
# only read it. config is the -dict list for set_property; key_files are files
# the configuration reads (e.g. a MIG .prj), hashed into the key as well and
# copied next to the IP; @DIR@ in config stands for that directory.
proc trx_ip {name vlnv config {key_files {}}} {
    # the part belongs in the key (a generated IP is part specific); the XC7A50T
    # keeps the key it always had, so the cache made before T53 stays valid
    set part [get_property PART [current_project]]
    set ptxt [expr {[string match xc7a50t* $part] ? "" : " $part"}]
    set dir [file join $::trx_cache ip_${name}_[trx_hash $key_files "$vlnv $config$ptxt"]]
    set config [string map [list @DIR@ $dir] $config]
    set xci [file join $dir $name $name.xci]
    if {![file exists [file join $dir done]]} {
        file delete -force $dir
        file mkdir $dir
        foreach f $key_files { file copy -force $f $dir }
        lassign [split $vlnv :] vendor library ipname
        create_ip -name $ipname -vendor $vendor -library $library -module_name $name -dir $dir
        set_property -dict $config [get_ips $name]
        generate_target {synthesis} [get_ips $name]
        synth_ip [get_ips $name]
        close [open [file join $dir done] w]
        puts "=== ip $name: built into $dir"
    } else {
        read_ip $xci
        puts "=== ip $name: from cache $dir"
    }
}

# Out-of-context synthesis of module $top, cached. read_sources is a script
# that reads every source of the block (it runs in a fresh in-memory project);
# key_files are all files whose contents decide the result. Returns the
# checkpoint; the caller reads the stub before synth_design of the full design
# and fills the black box with read_checkpoint -cell afterwards.
proc trx_ooc {top part key_files read_sources synth_args} {
    set key [trx_hash $key_files "$top $part $synth_args $read_sources"]
    set dcp  [file join $::trx_cache ooc_${top}_$key.dcp]
    set stub [file join $::trx_cache ooc_${top}_$key.stub.v]
    if {![file exists $dcp] || ![file exists $stub]} {
        create_project -in_memory -part $part
        set_property target_language Verilog [current_project]
        uplevel #0 $read_sources
        synth_design -top $top -part $part -mode out_of_context {*}$synth_args
        report_utilization -hierarchical -file $::trx_build/utilization_${top}_ooc.rpt
        write_checkpoint -force $dcp.tmp.[pid]
        write_verilog -force -mode synth_stub $stub.tmp.[pid]
        file rename -force $dcp.tmp.[pid] $dcp
        file rename -force $stub.tmp.[pid] $stub
        close_project
        puts "=== ooc $top: synthesized into $dcp"
    } else {
        puts "=== ooc $top: from cache $dcp"
    }
    return [list $dcp $stub]
}

# Every file below dir, sorted.
proc trx_tree {dir} {
    set out {}
    foreach f [glob -nocomplain -directory $dir *] {
        if {[file isdirectory $f]} {
            lappend out {*}[trx_tree $f]
        } else {
            lappend out $f
        }
    }
    return [lsort $out]
}
