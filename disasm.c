#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

uint32_t read32(FILE *fp)
{
	uint32_t ret;
	unsigned char buf[4];
	if (!fread(buf, 1, 4, fp)) {
		fprintf(stderr, "fread: %s", strerror(ferror(fp)));
		exit(1);
	}

	ret = buf[0];
	ret |= ((uint32_t)buf[1]) << 8;
	ret |= ((uint32_t)buf[2]) << 16;
	ret |= ((uint32_t)buf[3]) << 24;
	return ret;
}

uint64_t read64(FILE *fp)
{
	uint64_t ret;
	uint32_t buf[2];
	buf[0] = read32(fp);
	buf[1] = read32(fp);
	ret = buf[0];
	ret |= ((uint64_t)buf[1]) << 32;
	return ret;
}

float decode_fp20(uint32_t bits)
{
	/* FP20 is 1.6.13 - bias 31
	 * FP32 is 1.8.23 - bias 127
	 * The strategy is to expand FP20 to FP32, encode it as an int,
	 * then reinterpret it as a float
	 */

	union {
		float value;
		uint32_t bits;
	} u;

	int mantissa = bits & 0x1FFF;
	int exponent = (bits >> 13) & 0x3F;
	int sign = (bits >> 19) & 1;

	if (exponent == 63) /* inf/nan */
		exponent = 255;
	else
		exponent += 127 - 31; /* adjust bias */

	mantissa = mantissa << (23 - 13);
	/* TODO: round off ?
	mantissa |= mantissa >> 10; */

	u.bits = (sign << 31) | (exponent << 23) | mantissa;
	return u.value;
}

float decode_fix10(uint32_t bits)
{
	return (bits & 0x3ff) / 256.0f;
}

uint64_t embedded_consts;
static int embedded_consts_used;

const char *decode_operand(uint64_t bits)
{
	static char buf[32];
	char *dst = buf;
	int uniform = (bits >> 11) & 1;
	int reg = (bits >> 6) & 31;
	int x10 = (bits >> 3) & 1;

	if ((bits >> 1) & 1) {
		*dst = '-';
		++dst;
	}

	if ((bits >> 2) & 1) {
		strcpy(dst, "abs(");
		dst += 4;
	}

	if (reg >= 28) {
		/* r28-r30 = embedded constants, r31 = 0 or 1 */
		if (reg != 31) {
			int offset = 4 + ((bits >> 5) & 7) * 10;
			dst += sprintf(dst, "#%f", x10 ?
			    decode_fix10(embedded_consts >> offset) :
			    decode_fp20(embedded_consts >> offset));
			embedded_consts_used = 1;
		} else
			dst += sprintf(dst, "#%d", (bits >> 5) & 1);
	} else {
		/* normal registers */
		dst += sprintf(dst, "%c%c%d",
		    uniform ? 'u' : 'v',
		    x10 ? 'x' : 'r',
		    reg);
	}

	if ((bits >> 0) & 1)
		dst += sprintf(dst, " * #2");

	if ((bits >> 2) & 1) {
		*dst = ')';
		++dst;
	}


	return buf;
}

const char *decode_rd(uint64_t instr)
{
	static char buf[32];
	int reg = (instr >> 15) & 31;
	int x10 = ((instr >> 14) & 1) != ((instr >> 13) & 1);

	if (x10)
		sprintf(buf, "x%d", reg);
	else
		sprintf(buf, "r%d", reg);
	return buf;
}

void disasm_frag_alu_instr(uint64_t instr)
{
	char *sat;
	int opcode;

	printf(" %016"PRIx64" ", instr);

	if (instr == 0x3e41f200000fe7e8ull) {
		printf("NOP\n");
		return;
	}

	opcode = (instr >> 30) & 0x3;
	sat = (instr >> 24) & 1 ? "_SAT" : "";
	switch (opcode) {
	case 0x0: printf("FMA%s ", sat); break;
	case 0x1: printf("MIN%s ", sat); break;
	case 0x2: printf("MAX%s ", sat); break;
	case 0x3: printf("CSEL%s ", sat); break;
	default:
		printf("0x%x ", opcode);
	}

	printf("%s, ", decode_rd(instr));
	printf("%s, ", decode_operand(instr >> 0));
	printf("%s, ", decode_operand((instr >> 51) & ~1));
	printf("%s\n", decode_operand(instr >> 38));
}

void disasm_frag_alu_instrs(const uint64_t instrs[4])
{
	int i;
	embedded_consts = instrs[3];
	embedded_consts_used = 0;
	for (i = 0; i < 4; ++i) {
		if (i == 3 && embedded_consts_used)
			continue;

		printf("%d: ", i);
		disasm_frag_alu_instr(instrs[i]);
	}
}


void disasm_frag_alu(FILE *fp, int count)
{
	int i;
	for (i = 0; i < count; ++i) {
		uint64_t instrs[4];
		instrs[0] = read64(fp);
		instrs[1] = read64(fp);
		instrs[2] = read64(fp);
		instrs[3] = read64(fp);
		disasm_frag_alu_instrs(instrs);
	}
}

void disasm_fp(FILE *fp)
{
	int i;
	uint32_t hash, magic;
	uint32_t offset, len;
	char buf[8];

	hash = read32(fp);
	magic = read32(fp);

	if (magic == 0x26836d2f)
		printf("Fragment shader\n");
        else if (magic == 0x26836d1f)
		printf("Vertex shader\n");
	else {
		fprintf(stderr, "Error: %x\n", magic);
		exit(1);
	}

	fseek(fp, 0xe8, SEEK_SET);
	/* strange, offset is stored in words, while len is stored in bytes */
	offset = read32(fp) * 4;
	len = read32(fp);
	printf("code at %x..%x\n", offset, offset + len);

	fseek(fp, offset, SEEK_SET);
	fread(buf, 1, 8, fp);
	if (strncmp(buf, "AR20-BIN", 8)) {
		fprintf(stderr, "wrong header: '%s'\n", buf);
		exit(1);
	}

	fseek(fp, offset + 16, SEEK_SET);
	for (i = 16; i < len; i += 4) {
		uint32_t cmd = read32(fp);
		/* printf("cmd: %x\n", cmd); */
		if ((cmd & 0xffff0000) == 0x28040000) {
			int count = cmd & 0xffff;
			if (count % 8 != 0) {
				fprintf(stderr, "count %d not dividable by 8\n", count);
				exit(1);
			}
			printf("found ALU code at %x, %d instruction words\n", offset + i + 4, count / 8);
			disasm_frag_alu(fp, count / 8);
			i += 4 * count;
		}
	}
}

int main(int argc, char *argv[])
{
	int i;
	for (i = 1; i < argc; ++i) {
		FILE *fp = fopen(argv[i], "rb");
		if (!fp) {
			perror("fopen");
			exit(1);
		}

		disasm_fp(fp);
		fclose(fp);
	}
}
