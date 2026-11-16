"""Persistent local accounts, revocable sessions and encrypted Apple library links."""
from contextlib import contextmanager
from pathlib import Path
import hashlib
import os
import re
import secrets
import sqlite3
import threading
import time
from argon2 import PasswordHasher
from argon2.exceptions import VerificationError, InvalidHashError
from cryptography.fernet import Fernet
from fastapi import HTTPException

PASSWORDS=PasswordHasher()
DUMMY_HASH=PASSWORDS.hash(secrets.token_urlsafe(24))

def password_valid(password):
    if not isinstance(password,str) or not 12<=len(password)<=256:
        raise HTTPException(422,'Passwords must contain 12–256 characters')

def digest(value):return hashlib.sha256(value.encode()).hexdigest()

class Accounts:
    def __init__(self,settings):
        self.settings=settings
        self.root=settings.data_dir
        self.root.mkdir(parents=True,exist_ok=True,mode=0o700)
        self.path=self.root/'accounts.sqlite3'
        key_path=self.root/'apple-token.key'
        try:
            fd=os.open(key_path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
        except FileExistsError:pass
        else:
            with os.fdopen(fd,'wb') as f:f.write(Fernet.generate_key())
        self.cipher=Fernet(key_path.read_bytes())
        self.lock=threading.RLock()
        self.failures={}
        with self.db() as db:
            db.executescript('''
                CREATE TABLE IF NOT EXISTS users (
                    id INTEGER PRIMARY KEY, username TEXT UNIQUE NOT NULL,
                    password_hash TEXT NOT NULL, admin INTEGER NOT NULL DEFAULT 0,
                    disabled INTEGER NOT NULL DEFAULT 0, apple_token BLOB, storefront TEXT);
                CREATE TABLE IF NOT EXISTS sessions (
                    hash TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
                    csrf TEXT NOT NULL, expires INTEGER NOT NULL);
                CREATE TABLE IF NOT EXISTS apple_flows (
                    hash TEXT PRIMARY KEY, session_hash TEXT NOT NULL REFERENCES sessions(hash) ON DELETE CASCADE,
                    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE, expires INTEGER NOT NULL);
            ''')
            if not db.execute("SELECT 1 FROM users WHERE username='admin'").fetchone():
                password_valid(settings.admin_password)
                db.execute('INSERT INTO users(username,password_hash,admin) VALUES(?,?,1)',
                           ('admin',PASSWORDS.hash(settings.admin_password)))
        self.path.chmod(0o600)

    @contextmanager
    def db(self):
        with self.lock:
            db=sqlite3.connect(self.path)
            db.row_factory=sqlite3.Row
            db.execute('PRAGMA foreign_keys=ON')
            try:
                with db:yield db
            finally:db.close()

    def public(self,row):
        return {'id':row['id'],'username':row['username'],'admin':bool(row['admin']),
                'disabled':bool(row['disabled']),'apple_linked':bool(row['apple_token']),
                'storefront':row['storefront'] or self.settings.storefront}

    def get(self,user_id):
        with self.db() as db:row=db.execute('SELECT * FROM users WHERE id=?',(user_id,)).fetchone()
        if not row or row['disabled']:raise HTTPException(401,'Account unavailable')
        return dict(row)

    def authenticate(self,username,password,peer='api'):
        now=time.monotonic();bucket=(peer,username)
        with self.lock:
            count,start=self.failures.get(bucket,(0,now))
            if now-start>=300:count,start=0,now
            if count>=10:raise HTTPException(429,'Too many login attempts; try again later')
        with self.db() as db:row=db.execute('SELECT * FROM users WHERE username=?',(username,)).fetchone()
        try:valid=PASSWORDS.verify(row['password_hash'] if row else DUMMY_HASH,password)
        except (VerificationError,InvalidHashError):valid=False
        if not valid or not row or row['disabled']:
            with self.lock:
                if len(self.failures)>=2048:self.failures.pop(next(iter(self.failures)))
                count,start=self.failures.get(bucket,(0,now))
                if now-start>=300:count,start=0,now
                self.failures[bucket]=(count+1,start)
            raise HTTPException(401,'Invalid username or password')
        with self.lock:self.failures.pop(bucket,None)
        return dict(row)

    def login(self,user):
        token=secrets.token_urlsafe(32);csrf=secrets.token_urlsafe(32)
        with self.db() as db:
            db.execute('DELETE FROM sessions WHERE expires<?',(int(time.time()),))
            db.execute('INSERT INTO sessions VALUES(?,?,?,?)',(digest(token),user['id'],csrf,int(time.time())+43200))
        return token,csrf

    def session(self,token):
        with self.db() as db:
            row=db.execute('SELECT user_id,csrf FROM sessions WHERE hash=? AND expires>?',(digest(token),int(time.time()))).fetchone()
        if not row:raise HTTPException(401,'Sign in required')
        return self.get(row['user_id']),row['csrf']

    def logout(self,token):
        with self.db() as db:db.execute('DELETE FROM sessions WHERE hash=?',(digest(token),))

    def create(self,username,password):
        if not re.fullmatch(r'[a-z][a-z0-9_.-]{2,31}',username):raise HTTPException(422,'Use 3–32 lowercase letters, numbers, dots, dashes or underscores')
        password_valid(password)
        hashed=PASSWORDS.hash(password)
        try:
            with self.db() as db:
                cur=db.execute('INSERT INTO users(username,password_hash) VALUES(?,?)',(username,hashed))
                user_id=cur.lastrowid
        except sqlite3.IntegrityError:raise HTTPException(409,'Username already exists') from None
        return self.public(self.get(user_id))

    def list(self):
        with self.db() as db:return [self.public(row) for row in db.execute('SELECT * FROM users ORDER BY id')]

    def manage(self,user_id,password=None,disabled=None,delete=False):
        with self.db() as db:
            row=db.execute('SELECT * FROM users WHERE id=?',(user_id,)).fetchone()
            if not row:raise HTTPException(404,'Account not found')
            if row['admin']:raise HTTPException(403,'The admin account cannot be disabled, deleted or reset here')
            if delete:db.execute('DELETE FROM users WHERE id=?',(user_id,));return
            if password is not None:
                password_valid(password)
                db.execute('UPDATE users SET password_hash=? WHERE id=?',(PASSWORDS.hash(password),user_id))
            if disabled is not None:db.execute('UPDATE users SET disabled=? WHERE id=?',(int(disabled),user_id))
            db.execute('DELETE FROM sessions WHERE user_id=?',(user_id,))

    def change_password(self,user_id,password):
        password_valid(password)
        with self.db() as db:
            db.execute('UPDATE users SET password_hash=? WHERE id=?',(PASSWORDS.hash(password),user_id))
            db.execute('DELETE FROM sessions WHERE user_id=?',(user_id,))

    def start_link(self,user_id,session_token):
        state=secrets.token_urlsafe(32)
        with self.db() as db:
            db.execute('DELETE FROM apple_flows WHERE expires<?',(int(time.time()),))
            db.execute('DELETE FROM apple_flows WHERE user_id=?',(user_id,))
            db.execute('INSERT INTO apple_flows VALUES(?,?,?,?)',(digest(state),digest(session_token),user_id,int(time.time())+600))
        return state

    def consume_link(self,user_id,session_token,state):
        with self.db() as db:
            row=db.execute('DELETE FROM apple_flows WHERE hash=? AND user_id=? AND session_hash=? AND expires>? RETURNING hash',
                (digest(state),user_id,digest(session_token),int(time.time()))).fetchone()
        if not row:raise HTTPException(400,'Linking request expired or was already used; start again')

    def link(self,user_id,token,storefront):
        with self.db() as db:db.execute('UPDATE users SET apple_token=?,storefront=? WHERE id=?',
            (self.cipher.encrypt(token.encode()),storefront,user_id))

    def unlink(self,user_id):
        with self.db() as db:
            db.execute('UPDATE users SET apple_token=NULL,storefront=NULL WHERE id=?',(user_id,))
            db.execute('DELETE FROM apple_flows WHERE user_id=?',(user_id,))

    def apple_token(self,user):
        return self.cipher.decrypt(user['apple_token']).decode() if user['apple_token'] else None
