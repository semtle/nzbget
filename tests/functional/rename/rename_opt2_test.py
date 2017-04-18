nzbget_options = ['ParRename=no', 'RarRename=no', 'ParCheck=manual', 'DirectRename=yes']

def test_rename_obf1(nserv, nzbget):
	hist = nzbget.download_nzb('obfuscated1.nzb', unpack=True)
	assert hist['Status'] == 'SUCCESS/UNPACK'

def test_rename_obf2(nserv, nzbget):
	nzb_content = nzbget.load_nzb('obfuscated1.nzb')
	nzb_content = nzb_content.replace(';5mb.7z', ';abc')
	nzb_content = nzb_content.replace(';parrename', ';def')
	nzb_content = nzb_content.replace('.par2&', '&')
	hist = nzbget.download_nzb('obfuscated1-changed.nzb', nzb_content, unpack=True)
	assert hist['Status'] == 'SUCCESS/UNPACK'

def test_rename_obf3(nserv, nzbget):
	hist = nzbget.download_nzb('obfuscated2.nzb', unpack=True)
	assert hist['Status'] == 'SUCCESS/UNPACK'