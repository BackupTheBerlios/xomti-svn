memory validate off


proc f1 {n} {
    set r $n
    incr n -1
    while {$n >= 2} {
	set r [expr {$r*$n}]
	incr n -1
    }
    return $r
}

proc f2 {n} {
    if {$n == 2} {
	return 2
    } else {
	return [expr {[f2 [expr {$n-1}]] * $n}]
    }
}

set f3 [proc {} {f n} {
    if {$n == 2} {
	return 2
    } else {
	return [expr {[$f $f [expr {$n-1}]] * $n}]
    }
}]

time {f1 19} 10000
time {f2 19} 10000
time {$f3 $f3 19} 10000

proc add {a b} {
    return [expr {$a + $b}]
}

proc t1 {} {
    set r 1.0
    for {set i 100} {$i > 0} {incr i -1} {
	set r [add $r $r]
    }
    return $r
}

proc t2 {f} {
    set r 1.0
    for {set i 100} {$i > 0} {incr i -1} {
	set r [$f $r $r]
    }
    return $r
}


set aa [proc {} {a b} {
    return [expr {$a + $b}]
}]
proc t3 {aa} {
    set r 1.0
    for {set i 100} {$i > 0} {incr i -1} {
	set r [$aa $r $r]
    }
    return $r
}

time {t1} 100
time {t2 add} 100
time {t3 $aa} 100

proc v {} {
    return a
}

proc v1 {} {
    set l ""
    for {set i 1000} {$i > 0} {incr i -1} {
	append l [v]
    }
    return $l
}

proc v2 {v} {
    set l ""
    for {set i 1000} {$i > 0} {incr i -1} {
	append l [$v]
    }
    return $l
}

set vv [proc {} {} {return a}]

proc v3 {vv} {
    set l ""
    for {set i 1000} {$i > 0} {incr i -1} {
	append l [$vv]
    }
    return $l
}


time {v1} 100
time {v2 v} 100
time {v3 $vv} 100

proc v1b {} {
    return "D[v][v][v][v][v][v][v][v][v][v][v][v][v][v][v][v][v][v][v][v]"
}
proc v2b {v} {
    return "D[$v][$v][$v][$v][$v][$v][$v][$v][$v][$v][$v][$v][$v][$v][$v][$v][$v][$v][$v][$v]"
}
proc v3b {vv} {
    return "D[$vv][$vv][$vv][$vv][$vv][$vv][$vv][$vv][$vv][$vv][$vv][$vv][$vv][$vv][$vv][$vv][$vv][$vv][$vv][$vv]"
}

time {v1b} 1
time {v2b v} 1
time {v3b $vv} 1

time {v1b} 10000
time {v2b v} 10000
time {v3b $vv} 10000


