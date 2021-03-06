#include <iostream>
#include <vector>
#include <bitset>
#include <cmath>
#include <array>
#include <cassert>


using namespace std;


class Checker {
public:
    Checker(int nx, int ny)
        : nx(nx), ny(ny), data(nx * ny, -1)
    {
    }
    bool check(int i, int j, int t) {
        int& val = data.at(i + nx * j);
        auto is = {-1, 0, 1};
        auto js = {-1, 0, 1};
        for (int iss : is) {
            for (int jss : js) {
                int ii = i + iss;
                int jj = j + jss;
                if (ii < 0 || jj < 0 || ii >= nx || jj >= ny) continue;
                int vval = data.at(ii + nx * jj);
                if (vval < t - 1) {
                    cerr << "vval: " << vval << endl;
                    cerr << "nx: " << nx << endl;
                    cerr << "ny: " << ny << endl;
                    cerr << "ii: " << ii << endl;
                    cerr << "jj: " << jj << endl;
                    cerr << "i: " << i << endl;
                    cerr << "j: " << j << endl;
                    cerr << "t: " << t << endl;
                    return false;
                }
            }
        }
        if (val == t - 1) {
            val = t;
            return true;
        } else {
            return false;
        }
    }
private:    
    int nx, ny;
    std::vector<int> data;
};


template <typename T>
inline T pow2(T k) {
    return 1ll << k;
}

template <typename F>
void tiledLoops(int nx, int ny, int nt, F func) {
    std::array<int8_t,24> lut_ii{
        0, 1,-1, 0, 0, 1, 1,-1,-1, 1,-1, 0, 0, 0, 1,-1, 0, 0, 0, 0, 0, 0, 1,-1
    };
    std::array<int8_t,24> lut_jj{
        0, 0, 0, 1,-1, 1,-1, 1,-1, 0, 0, 1,-1, 0, 0, 0, 0, 1,-1, 1,-1, 0, 0, 0
    };
    std::array<int8_t,24> lut_tt{
        0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1
    };
    std::array<int8_t,24> lut_mode {
        0,19,19,14,14, 0, 0, 0, 0,14,14,19,19, 0,14,14, 0,14,14,19,19, 0,19,19
    };
    
    int base = max(nx, ny);
    base = base | 1; // round up to the nearest odd number
    int height = nt;
    
    int32_t tileParam = 1;
    while (pow2(tileParam+1) < base + height - 1) {
        tileParam++;
    }
    assert(pow2(tileParam+1) >= base + height - 1);
    //cerr << "tileParam: " << tileParam << endl;
    
    int32_t sx = - base / 2;
    //cerr << "sx: " << sx << endl;
    int32_t ex = sx + nx - 1;
    //cerr << "ex: " << ex << endl;
    
    int32_t sy = - base / 2;
    //cerr << "sy: " << sy << endl;
    int32_t ey = sy + ny - 1;
    //cerr << "ey: " << ey << endl;
    
    int32_t st = + base / 2;
    //cerr << "st: " << st << endl;
    int32_t et = st + nt - 1;
    //cerr << "et: " << et << endl;
    
    // size of array is the logarithm of desired tile size
    std::vector<int8_t> state(tileParam);
    
    // height of the tile depends on the number of iterations:
    // num of iterations: 2^{3i+1}-2^i
    // i from 1 to inf
    // height big: 2^{i+1} ( range: [0;2^{i+1}-1] )
    // height small: 2^i ( range: [0;2^i-1] )
    // width: 2^{i+1} - 1 ( range: [-(2^i - 1); +(2^i - 1)] )
    int64_t iterations = pow2(3ll * tileParam + 1ll) - pow2(tileParam);
    //iterations = 20;
    //cerr << "iterations: " << iterations << endl;
    
    std::vector<int32_t> tts(tileParam + 1);
    std::vector<int32_t> iis(tileParam + 1);
    std::vector<int32_t> jjs(tileParam + 1);
    
    size_t K = state.size() - 1;
    
    bool finished = false;
    while (1) {

        //cerr << "=====" << endl;
        // step

        //for (int i = 0; i < state.size(); i++) {
        //    cerr << int(state[i]) << " ";
        //}
        //cerr << endl;

        bool skipTile = false;
        while (1) {
            int32_t ss = state[K];

            int32_t tt = lut_tt[ss];
            int32_t ii = lut_ii[ss];
            int32_t jj = lut_jj[ss];

            tts[K] = tts[K+1] + (tt << K);
            iis[K] = iis[K+1] + (ii << K);
            jjs[K] = jjs[K+1] + (jj << K);

            int32_t min_t = tts[K] + 0;
            int32_t min_x = iis[K] - (pow2(K + 1) - 1);
            int32_t min_y = jjs[K] - (pow2(K + 1) - 1);
            
            int32_t mode = lut_mode[ss];
            int32_t height = pow2(K + (mode == 0 ? 2 : 1)) - 1;

            int32_t max_t = tts[K] + height - 1;
            int32_t max_x = iis[K] + (pow2(K + 1) - 1);
            int32_t max_y = jjs[K] + (pow2(K + 1) - 1);

            //cerr 
            //    << min_x << " "
            //    << max_x << " "
            //    << min_y << " "
            //    << max_y << " "
            //    << min_t << " "
            //    << max_t << " "
            //    << endl;
            
            if (max_t < st || min_t > et ||
                max_x < sx || min_x > ex || 
                max_y < sy || min_y > ey) 
            {
                skipTile = true;
                break;
            }

            if (K == 0) break; 

            state[K-1] = lut_mode[state[K]];
            
            K--;
        };

        //cerr << "skipTile: " << skipTile << endl;
        //cerr << "ijt: " << iis[0] << " " << jjs[0] << " " << tts[0] << endl;

        if (!skipTile) {

            // print
            if (sx <= iis[0] && iis[0] <= ex &&
                sy <= jjs[0] && jjs[0] <= ey) {
                if (st <= tts[0] && tts[0] <= et) {
                    func(iis[0] - sx, jjs[0] - sy, tts[0] - st);
                }
                if (lut_mode[state[0]] == 0) {
                    if(st <= tts[0] + 1 && tts[0] + 1 <= et) {
                        func(iis[0] - sx, jjs[0] - sy, tts[0] + 1 - st);
                    }
                }
            }

        } 

        while (state[K] == 13 || state[K] == 18 || state[K] == 23) {
            K++;
            if (K == state.size()) { finished = true; break; }
        }
        if (finished == true) break;

        state[K]++;


    }
     
}

int main() {
    int nx = 30;
    int ny = 30;
    int nt = 30;
    
    Checker checker(nx, ny);

    volatile int unused = 0;

    auto f = [&unused,&checker](int i, int j, int t) {
        std::cout << i << " " << j << " " << t << endl;
        //cerr << "ijt: " << i << " " << j << " " << t << endl;
        assert(checker.check(i, j, t));
        unused += i + j + t;
    };
    tiledLoops(nx, ny, nt, f);

    cerr << "unused: " << unused << endl;
}
