(set-info :smt-lib-version 2.6)
(set-logic QF_SLIA)
(set-info :status sat)
(declare-fun x () String)
(assert (str.in_re (str.++ "bbbbbbbb" x) (re.* (re.++ (str.to_re "bbbb") (re.* (str.to_re "aaaa"))))))
(check-sat)
