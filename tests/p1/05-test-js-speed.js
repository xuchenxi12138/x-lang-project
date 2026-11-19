let t1=Date.now()
let count=0
for(let i=0;i<1000000000;i++){
    count++
}
let t2=Date.now()
let t3=t2-t1
console.log(t3," ",count)