@ Due to the way mks4agb (Nintendo's tool) works, key split table labels can
@ appear before the actual start of the key split table data. If you look at
@ the first keysplit table (keysplit_piano), you'll notice it's offset backwards
@ by 36 bytes. This is because the key split tables don't map instruments
@ for the entire note range (0-127)--they only map subsets, and the upper
@ and lower ranges aren't necessarily 0 or 127.
@
@ For example if a key split table maps an note range of 10-20, then the key
@ split table label will be offset 10 bytes before the actual key split data
@ begins. Therefore, the notes naturally map to the key split table without
@ any extra offset calculation.

keysplit piano, 36
	split 0, 55
	split 1, 70
	split 2, 91
	split 3, 108

keysplit strings, 36
	split 0, 69
	split 1, 81
	split 2, 108

keysplit trumpet, 36
	split 0, 66
	split 1, 84
	split 2, 108

keysplit tuba, 24
	split 0, 42
	split 1, 108

keysplit french_horn, 36
	split 0, 66
	split 1, 108

keysplit bw_keysplit_0, 0
    split 0, 70
    split 1, 77
    split 2, 128

keysplit bw_keysplit_10, 0
    split 0, 80
    split 1, 128

keysplit bw_keysplit_29, 0
    split 0, 43
    split 1, 128

keysplit bw_keysplit_30, 0
    split 0, 66
    split 1, 75
    split 2, 82
    split 3, 128

keysplit bw_keysplit_35, 0
    split 0, 75
    split 1, 127

keysplit bw_keysplit_36, 0
    split 0, 55
    split 1, 67
    split 2, 128

keysplit bw_keysplit_39, 0
    split 0, 65
    split 1, 74
    split 2, 128

keysplit bw_keysplit_40, 0
    split 0, 48
    split 1, 127

keysplit bw_keysplit_42, 0
    split 0, 59
    split 1, 68
    split 2, 128

keysplit bw_keysplit_44, 0
    split 0, 77
    split 1, 127

keysplit bw_keysplit_56, 0
    split 0, 65
    split 1, 74
    split 2, 127

keysplit bw_keysplit_90, 0
    split 0, 55
    split 1, 63
    split 2, 70
    split 3, 128
