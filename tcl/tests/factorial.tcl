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

set x [proc {} {n} {
    if {$n == 2} {
	return 2
    } else {
	return [expr {[::$x [expr {$n-1}]] * $n}]
    }
}]

proc add1 {a b} {
    return [expr {$a + $b}]
}

proc t1 {} {
    set r 1.0
    for {set i 1000} {$i > 0} {incr i -1} {
	set r [add1 $r $r]
    }
    return $r
}

proc t2 {} {
    set r 1.0
    for {set i 1000} {$i > 0} {incr i -1} {
	set r [[proc {} {a b} {return [expr {$a + $b}]}] $r $r]
    }
    return $r
}

proc t3 {} {
    set r 1.0
    set f [proc {} {a b} {return [expr {$a + $b}]}]
    for {set i 100} {$i > 0} {incr i -1} {
	set r [$f $r $r]
	set r [$f $r $r]
	set r [$f $r $r]
	set r [$f $r $r]
	set r [$f $r $r]
	set r [$f $r $r]
	set r [$f $r $r]
	set r [$f $r $r]
	set r [$f $r $r]
	set r [$f $r $r]
    }
    return $r
}
