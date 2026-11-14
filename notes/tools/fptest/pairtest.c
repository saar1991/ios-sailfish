#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef float GLfloat;
#define FAST_RECTS 16
struct fast_attr { int dummy; };
static GLfloat V[16][4];
static void fast_attr_read(const struct fast_attr *a, int i, GLfloat v[4]) { (void)a; memcpy(v, V[i], sizeof(V[0])); }
static int
fast_pair(const struct fast_attr *ap, int *idx, int nrect)
{
	int ntri = 2 * nrect, used[2 * FAST_RECTS] = {0}, out[6 * FAST_RECTS], n = 0;
	GLfloat pos[6 * FAST_RECTS][4];
	for (int i = 0; i < 3 * ntri; i++) {
		fast_attr_read(ap, idx[i], pos[i]);
	}
	for (int a = 0; a < ntri; a++) {
		if (used[a]) {
			continue;
		}
		int mate = -1;
		for (int b = a + 1; b < ntri && mate < 0; b++) {
			if (used[b]) {
				continue;
			}
			int same = 0, other = -1;
			for (int q = 0; q < 3; q++) {
				int hit = 0;
				for (int p = 0; p < 3; p++) {
					hit |= memcmp(pos[3 * a + p], pos[3 * b + q], sizeof(pos[0])) == 0;
				}
				same += hit;
				other = hit ? other : 3 * b + q;
			}
			if (same != 2) {
				continue;
			}
			/* the partner across the diagonal: the four corners lie on two x and two y values */
			const GLfloat *c4[4] = {pos[3 * a], pos[3 * a + 1], pos[3 * a + 2], pos[other]};
			int nx = 0, ny = 0;
			GLfloat xs[4], ys[4];
			for (int k = 0; k < 4; k++) {
				int fx = 0, fy = 0;
				for (int m = 0; m < nx; m++) {
					fx |= xs[m] == c4[k][0];
				}
				for (int m = 0; m < ny; m++) {
					fy |= ys[m] == c4[k][1];
				}
				if (!fx) {
					xs[nx++] = c4[k][0];
				}
				if (!fy) {
					ys[ny++] = c4[k][1];
				}
			}
			mate = nx == 2 && ny == 2 ? b : -1;
		}
		if (mate < 0) {
			return -1;
		}
		used[a] = used[mate] = 1;
		for (int p = 0; p < 3; p++) {
			out[n++] = idx[3 * a + p];
		}
		for (int p = 0; p < 3; p++) {
			out[n++] = idx[3 * mate + p];
		}
	}
	memcpy(idx, out, sizeof(int) * (size_t)n);
	return 0;
}
int main(void) {
	float xs[4] = {0, 3, 10, 13}, ys[4] = {0, 4, 20, 24};
	for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { V[r * 4 + c][0] = xs[c]; V[r * 4 + c][1] = ys[r]; V[r * 4 + c][3] = 1; }
	int tris[18][3], n = 0;
	for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
		int a = r * 4 + c, b = a + 1, d = a + 4, e = d + 1;
		tris[n][0] = a; tris[n][1] = b; tris[n][2] = d; n++;   /* one diagonal */
		tris[n][0] = b; tris[n][1] = e; tris[n][2] = d; n++;
	}
	int fails = 0;
	for (int trial = 0; trial < 2000; trial++) {
		int order[18]; for (int i = 0; i < 18; i++) order[i] = i;
		for (int i = 17; i > 0; i--) { int k = rand() % (i + 1), t = order[i]; order[i] = order[k]; order[k] = t; }
		int idx[96]; for (int i = 0; i < 18; i++) for (int p = 0; p < 3; p++) idx[3 * i + p] = tris[order[i]][p];
		struct fast_attr ap;
		if (fast_pair(&ap, idx, 9) != 0) { fails++; continue; }
		/* each consecutive pair must be the two triangles of one cell: 4 distinct corners on 2 x and 2 y */
		for (int q = 0; q < 9; q++) {
			float mnx = 1e9, mxx = -1e9, mny = 1e9, mxy = -1e9;
			for (int p = 0; p < 6; p++) { float *v = V[idx[6 * q + p]]; mnx = v[0] < mnx ? v[0] : mnx; mxx = v[0] > mxx ? v[0] : mxx; mny = v[1] < mny ? v[1] : mny; mxy = v[1] > mxy ? v[1] : mxy; }
			int okc = 0; for (int c = 0; c < 3; c++) okc |= mnx == xs[c] && mxx == xs[c + 1];
			int okr = 0; for (int r = 0; r < 3; r++) okr |= mny == ys[r] && mxy == ys[r + 1];
			if (!okc || !okr) { fails++; break; }
		}
	}
	printf("nine-slice pairing: 2000 shuffled orders, %d failures\n", fails);
	return fails != 0;
}
