	.text
	.set	noreorder
foo:
	lwc2	$0, 0($0)
	lwc2	$1, 0($0)
	lwc2	$2, 0($0)
	lwc2	$3, 0($0)
	lwc2	$4, 0($0)
	lwc2	$5, 0($0)
	lwc2	$6, 0($0)
	lwc2	$7, 0($0)
	lwc2	$8, 0($0)
	lwc2	$9, 0($0)
	lwc2	$10, 0($0)
	lwc2	$11, 0($0)
	lwc2	$12, 0($0)
	lwc2	$13, 0($0)
	lwc2	$14, 0($0)
	lwc2	$15, 0($0)
	lwc2	$16, 0($0)
	lwc2	$17, 0($0)
	lwc2	$18, 0($0)
	lwc2	$19, 0($0)
	lwc2	$20, 0($0)
	lwc2	$21, 0($0)
	lwc2	$22, 0($0)
	lwc2	$23, 0($0)
	lwc2	$24, 0($0)
	lwc2	$25, 0($0)
	lwc2	$26, 0($0)
	lwc2	$27, 0($0)
	lwc2	$28, 0($0)
	lwc2	$29, 0($0)
	lwc2	$30, 0($0)
	lwc2	$31, 0($0)

	swc2	$0, 0($0)
	swc2	$1, 0($0)
	swc2	$2, 0($0)
	swc2	$3, 0($0)
	swc2	$4, 0($0)
	swc2	$5, 0($0)
	swc2	$6, 0($0)
	swc2	$7, 0($0)
	swc2	$8, 0($0)
	swc2	$9, 0($0)
	swc2	$10, 0($0)
	swc2	$11, 0($0)
	swc2	$12, 0($0)
	swc2	$13, 0($0)
	swc2	$14, 0($0)
	swc2	$15, 0($0)
	swc2	$16, 0($0)
	swc2	$17, 0($0)
	swc2	$18, 0($0)
	swc2	$19, 0($0)
	swc2	$20, 0($0)
	swc2	$21, 0($0)
	swc2	$22, 0($0)
	swc2	$23, 0($0)
	swc2	$24, 0($0)
	swc2	$25, 0($0)
	swc2	$26, 0($0)
	swc2	$27, 0($0)
	swc2	$28, 0($0)
	swc2	$29, 0($0)
	swc2	$30, 0($0)
	swc2	$31, 0($0)

# Force some (non-delay-slot) zero bytes, to make 'objdump' print ...
	.align	4, 0
	.space	16
