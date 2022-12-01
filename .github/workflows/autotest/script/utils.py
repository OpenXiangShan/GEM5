import configparser
from logging import warning
import re
import os
import git
import json
import psutil
import time
import glob
import multiprocessing

# 初始化线程池(实际上是核数)

manger = multiprocessing.Manager()
tpoolId = manger.dict({0:[]})
tlock = manger.Lock()
tcfgfile = {}

def tpool_init(cfgfile):
    global tpoolId,tlock,tcfgfile
    tcfgfile = cfgfile
    srange = cfgfile['iteration']['srange'].split(',')
    
    tpoolId = manger.dict({0:[i for i in range(int(srange[0]), int(srange[-1]))]})
    tlock = manger.Lock()

def __dy_alloc(n):
    temp_flag = False
    while True:
        tlock.acquire()
        # To avoid potential conflicts, we allow CI to use SMT.
        num_logical_core = psutil.cpu_count(logical=False)
        core_usage = psutil.cpu_percent(interval=1, percpu=True)
        num_window = num_logical_core // n
        for i in range(num_window):
            window_usage = core_usage[i * n: i * n + n]
            if sum(window_usage) < 0.3 * n and True not in map(lambda x: x > 0.5, window_usage):
                tlock.release()
                return ((i * n) % 128) // 64, (i * n, i * n + n - 1)
        if not temp_flag:
            temp_flag = True
            print('no free cores found. will wait for free cores')
        tlock.release()
        time.sleep(1)
        
#numa_args = f"numactl -m {numa_info[0]} -C {numa_info[1]}-{numa_info[2]}"
def __st_alloc(n):
    global tpoolId, tlock, tcfgfile
    temp_flag=False
    while True:
        tlock.acquire()
        if len(tpoolId[0]) >= n:
            tpoolId[0].sort()
            alloced = [tpoolId[0][0]]
            if n == 1:
                tpoolId[0] = tpoolId[0][1:]
                tlock.release()
                return (alloced[0],alloced[-1])
            st = 0
            for i in range(1,len(tpoolId[0])):
                if tpoolId[0][i-1]+1 == tpoolId[0][i]:
                    alloced.append(tpoolId[0][i])
                    if len(alloced) == n:
                        tpoolId[0] = tpoolId[0][:st] + tpoolId[0][i+1:]
                        tlock.release()
                        return (alloced[0], alloced[-1])
                else:
                    alloced = [tpoolId[0][i]]
                    st = i
        if not temp_flag:
            temp_flag = True
            print('no free cores found. will wait for free cores')
        tlock.release()
        time.sleep(1)
            
def tpool_alloc(n):
    if n is None :
        warning("can't find the numacores specArg,using numacores=0")
        return '', [-1, -1]
    if int(n) < 1:
        return '',[-1,-1]
    numa_args=''
    M=None
    C=None
    if tcfgfile['iteration']['smode'] == 'st':
        C = __st_alloc(int(n))
        M = str(((C[0]) % 128) // 64)
    elif tcfgfile['iteration']['smode'] == 'dy':
        M,C = __dy_alloc(int(n))
    else:
        print("error iteration-smode")
        exit(-1)
    numa_args = f"numactl -m {M} -C {C[0]}-{C[1]}"
    return numa_args , C
def tpool_free(n):
    global tpoolId, tlock, tcfgfile
    if n[0] < 0:
        return
    if tcfgfile['iteration']['smode'] == 'st':
        tlock.acquire()
        for i in range(n[0],n[1]+1):
            tpoolId[0]+=[i]
        tlock.release()


class CFGReader:
    cfg_map = {'global': {}}

    def __init__(self, cfg_path) -> None:
        if not os.path.exists(cfg_path):
            print('there is no cfg file')
            exit(1)
        cfgfile = configparser.ConfigParser()
        try:
            cfgfile.read(cfg_path)
        except Exception as e:
            print(e)
            exit(-1)
        reObj = re.compile('\{[^\{.]*\}')
        # 查找段
        for section in cfgfile.sections():
            self.cfg_map.update({section: {}})
            # 查找段的每个设置
            for options in cfgfile.items(section):
                result = options[1]
                # 替换变量
                for i in reObj.findall(result):
                    rep = i[1:-1]
                    var0 = self.cfg_map['global'].get(rep)
                    if var0:
                        result = result.replace(i, var0)
                    var1 = self.cfg_map[section].get(rep)
                    if var1:
                        result = result.replace(i, var1)
                    if not (var0 or var1):
                        warning(
                            'cfgReader: [can\'t fint the var] ' + section + ":" + rep)
                self.cfg_map[section].update({options[0]: result})

    def __getitem__(self, index):
        return self.cfg_map[index]

    def items(self):
        return self.cfg_map.items()


def splitfile(str: str, i):
    if i <= 0:
        i = 1
    subidx = len(str)
    if i >= subidx:
        print("the file parser error!")
        exit(-1)
    for _ in range(i):
        subidx = str.rfind('/', 0, subidx)-1
        if subidx < 0:
            print("the file parser error!")
            exit(-1)
    return str[subidx+2:]

# 返回文件名和子log文件路径
def get_file_list(path: str):
    subpaths = path.split(';')
    sargs = []
    sublogs = []
    # 获取所有的文件行
    lines: list[str] = []
    for subpath in subpaths:
        if subpath.endswith('.paths'):
            with open(subpath, 'r') as fs:
                lines += fs.read().split('\n')
        else:
            lines.append(subpath)
    for dual in lines:
        dual = dual.strip()
        dual = ' '.join(dual.split())
        # 参数输入模式,格式:'参数 log路径 0(必选) '
        #比如: -d=0 -w=1 test 0
        if len(dual.split(' ')) > 2:
            info = dual[0:dual.rfind(' ')]
            idx = info.rfind(' ')
            name = info[idx+1:0]
            info = info[0:idx]
            sargs.append(info)
            sublogs.append(name)
            continue
        #路径搜索模式,格式:'bin路径 分类级数'
        #比如: test/binfile.bin 2
        if dual == '':
            continue
        idx = dual.rfind(' ')
        info = dual 
        ser = 1
        if idx > 0 and dual[idx+1:].isdigit():
            info = dual[0:idx]
            ser = int(dual[idx+1:])
        for file in glob.glob(info):
            if os.path.exists(file):
                sargs.append(file)
            else:
                warning("can\'t find the file:"+file)
            name = splitfile(file, int(ser))
            # 去掉后缀名
            name = os.path.splitext(name)[0]
            sublogs.append(name)
    return sargs, sublogs





def free_numa_cores(n):
    pass


def getBranch(cfgfile):
    '''
    pull repo and checkout
    '''
    try:
        repo = git.Repo(path=cfgfile['global']['working_dir'])
    except:
        print('can\'t to load the repo,will to pull new here')
        try:
            repo = git.Repo.clone_from(
                url=cfgfile['global']['repo_url'], to_path=cfgfile['global']['working_dir'])
        except:
            print('cant find the repo,exit')
            exit(-1)
    try:
        repo.git.checkout(cfgfile['global']['repo_branch'])
        repo.git.pull()
    except:
        print('repo init error,exit')
        exit(-1)
    return repo
# return the all commit info


def getAllCommitInfo(cfgfile, repo: git.Repo, info: str):
    '''
    get branch's commits info:commit:hashcode,author,summary,date
    Sort by time
    '''
    repo.git.checkout(cfgfile['global']['repo_branch'])
    repo.git.pull()
    real_log_list = []
    if info.isdigit():
        commit_log = repo.git.log(
            '--pretty={"commit":"%h","author":"%an","summary":"%s","date":"%cd"}', max_count=int(info), date='format:%Y-%m-%d %H:%M')
        log_list = commit_log.split("\n")
        real_log_list = [eval(item) for item in log_list]
    else:
        commits = info.split(';')
        for commit in commits:
            real_log_list.append({
                "commit": commit,
                "author": "None",
                "summary": "None",
                "date": "None"
            })
    return real_log_list


def checkCommit(commit_info_path, origin_commits):
    '''
    compare the local commits info with origin commits
    and return the extra commit
    if local is the newest ,return None
    '''
    extra_commits = []
    # init commits info
    with open(commit_info_path, 'w') as fs:
        fs.write(json.dumps(origin_commits))
    if not os.path.exists(commit_info_path+'.old'):
        return origin_commits
    ##
    local_commits = []
    # load last has done commit
    with open(commit_info_path+'.old', 'r') as fs:
        local_commits = json.loads(fs.read())

    if len(local_commits) == 0:
        return origin_commits

    for i in range(len(origin_commits)):
        if origin_commits[i]['commit'] == local_commits[0]['commit']:
            if i == 0:
                return []
            extra_commits = origin_commits[0:i]
            return extra_commits
    return origin_commits


def saveCommits(commit_info_path):
    '''
    save has finished commit
    '''
    os.rename(commit_info_path, commit_info_path+'.old')


# get cfgfile custom works
# return [[pre-task,task,post-task]]
def getWorks(cfgfile):
    pre_work: dict[str, list] = {}
    works: dict[str, list] = {}
    post_work: dict[str, list] = {}
    for work in cfgfile.items():
        if not work[1].get('pre-task'):
            work[1].update({'pre-task': ''})
        if not work[1].get('post-task'):
            work[1].update({'post-task': ''})
        if not work[1].get('except-task'):
            work[1].update({'except-task': ''})
        if work[0] == 'pre-work':
            pre_work.update({'pre-work': [work[1].get('pre-task'),
                                          work[1].get('task'),
                                          work[1].get('post-task'),
                                          work[1].get('except-task')]})
        if work[0] == 'post-work':
            post_work.update({'post-work': [work[1].get('pre-task'),
                                            work[1].get('task'),
                                            work[1].get('post-task'),
                                            work[1].get('except-task')]})
        if work[0].startswith('work-'):
            works.update({work[0][5:]: [work[1].get('pre-task'),
                                        work[1].get('task'),
                                        work[1].get('post-task'),
                                        work[1].get('except-task')]})
    return (works, pre_work, post_work)


def argReplace(coms, specArg: dict):
    reObj = re.compile('\$[^\$.]*\$')
    for i in range(len(coms)):
        if coms[i]:
            for arg in reObj.findall(coms[i]):
                rep = arg[1:-1]
                var0 = str(specArg.get(rep))
                if var0 is not None:
                    coms[i] = coms[i].replace(arg, var0)
                else:
                    warning('argReplace: [can\'t find the specarg] ' + rep)

# start one work
# return finished
